// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2023 NeoN authors

#include "NeoN/core/parallelAlgorithms.hpp"
#include "NeoN/finiteVolume/cellCentred/operators/gaussGreenDiv.hpp"

namespace NeoN::finiteVolume::cellCentred
{

/* @brief free standing function implementation of the divergence operator
** ie computes 1/V \sum_f S_f \cdot \phi_f
** where S_f is the face normal flux of a given face
**  phi_f is the face interpolate value
**
**
** @param faceFlux
** @param neighbour - mapping from face id to neighbour cell id
** @param owner - mapping from face id to owner cell id
** @param faceCells - mapping from boundary face id to owner cell id
*/
template<typename ValueType>
void computeDiv(
    const Executor& exec,
    localIdx nInternalFaces,
    localIdx nBoundaryFaces,
    View<const localIdx> neighbour,
    View<const localIdx> owner,
    View<const localIdx> faceCells,
    View<const scalar> faceFlux,
    View<const ValueType> phiF,
    View<const scalar> v,
    View<ValueType> res,
    const dsl::Coeff operatorScaling
)
{
    auto nCells = v.size();
    // check if the executor is GPU
    if (std::holds_alternative<SerialExecutor>(exec))
    {
        for (localIdx i = 0; i < nInternalFaces; i++)
        {
            ValueType flux = faceFlux[i] * phiF[i];
            res[owner[i]] += flux;
            res[neighbour[i]] -= flux;
        }

        for (localIdx i = nInternalFaces; i < nInternalFaces + nBoundaryFaces; i++)
        {
            auto own = faceCells[i - nInternalFaces];
            ValueType valueOwn = faceFlux[i] * phiF[i];
            res[own] += valueOwn;
        }

        // TODO does it make sense to store invVol and multiply?
        for (localIdx celli = 0; celli < nCells; celli++)
        {
            res[celli] *= operatorScaling[celli] / v[celli];
        }
    }
    else
    {
        parallelFor(
            exec,
            {0, nInternalFaces},
            KOKKOS_LAMBDA(const localIdx i) {
                ValueType flux = faceFlux[i] * phiF[i];
                Kokkos::atomic_add(&res[owner[i]], flux);
                Kokkos::atomic_sub(&res[neighbour[i]], flux);
            },
            "sumFluxesInternal"
        );

        parallelFor(
            exec,
            {nInternalFaces, nInternalFaces + nBoundaryFaces},
            KOKKOS_LAMBDA(const localIdx i) {
                auto own = faceCells[i - nInternalFaces];
                ValueType valueOwn = faceFlux[i] * phiF[i];
                Kokkos::atomic_add(&res[own], valueOwn);
            },
            "sumFluxesInternal"
        );

        parallelFor(
            exec,
            {0, nCells},
            KOKKOS_LAMBDA(const localIdx celli) {
                res[celli] *= operatorScaling[celli] / v[celli];
            },
            "normalizeFluxes"
        );
    }
}

template<typename ValueType>
void computeDivExp(
    const SurfaceField<scalar>& faceFlux,
    const VolumeField<ValueType>& phi,
    const SurfaceInterpolation<ValueType>& surfInterp,
    Vector<ValueType>& divPhi,
    const dsl::Coeff operatorScaling
)
{
    const UnstructuredMesh& mesh = phi.mesh();
    const auto exec = phi.exec();
    SurfaceField<ValueType> phif(
        exec, "phif", mesh, createCalculatedBCs<SurfaceBoundary<ValueType>>(mesh)
    );
    // TODO: remove or implement
    // fill(phif.internalVector(), NeoN::zero<ValueType>::value);
    surfInterp.interpolate(faceFlux, phi, phif);

    // TODO: currently we just copy the boundary values over
    phif.boundaryData().value() = phi.boundaryData().value();

    auto nInternalFaces = mesh.nInternalFaces();
    auto nBoundaryFaces = mesh.nBoundaryFaces();
    computeDiv<ValueType>(
        exec,
        nInternalFaces,
        nBoundaryFaces,
        mesh.faceNeighbour().view(),
        mesh.faceOwner().view(),
        mesh.boundaryMesh().faceCells().view(),
        faceFlux.internalVector().view(),
        phif.internalVector().view(),
        mesh.cellVolumes().view(),
        divPhi.view(),
        operatorScaling

    );
}

#define NF_DECLARE_COMPUTE_EXP_DIV(TYPENAME)                                                       \
    template void computeDivExp<TYPENAME>(                                                         \
        const SurfaceField<scalar>&,                                                               \
        const VolumeField<TYPENAME>&,                                                              \
        const SurfaceInterpolation<TYPENAME>&,                                                     \
        Vector<TYPENAME>&,                                                                         \
        const dsl::Coeff                                                                           \
    )

NF_DECLARE_COMPUTE_EXP_DIV(scalar);
NF_DECLARE_COMPUTE_EXP_DIV(Vec3);


template<typename ValueType>
void computeDivImp(
    la::LinearSystem<ValueType, localIdx>& ls,
    const SurfaceField<scalar>& faceFlux,
    const VolumeField<ValueType>& phi,
    const dsl::Coeff operatorScaling,
    const SparsityPattern& sparsityPattern
)
{
    const UnstructuredMesh& mesh = phi.mesh();
    const auto nInternalFaces = mesh.nInternalFaces();
    const auto exec = phi.exec();

    const auto [sFaceFlux, owner, neighbour, surfFaceCells, diagOffs, ownOffs, neiOffs] = views(
        faceFlux.internalVector(),
        mesh.faceOwner(),
        mesh.faceNeighbour(),
        mesh.boundaryMesh().faceCells(),
        sparsityPattern.diagOffset(),
        sparsityPattern.ownerOffset(),
        sparsityPattern.neighbourOffset()
    );
    auto [matrix, rhs] = ls.view();

    parallelFor(
        exec,
        {0, nInternalFaces},
        KOKKOS_LAMBDA(const localIdx facei) {
            scalar flux = sFaceFlux[facei];
            // scalar weight = 0.5;
            scalar weight = flux >= 0 ? 1 : 0;
            ValueType value = zero<ValueType>();
            auto own = owner[facei];
            auto nei = neighbour[facei];

            // add neighbour contribution upper
            auto rowNeiStart = matrix.rowOffs[nei];
            auto rowOwnStart = matrix.rowOffs[own];

            scalar operatorScalingNei = operatorScaling[nei];
            scalar operatorScalingOwn = operatorScaling[own];

            value = -weight * flux * one<ValueType>();
            // scalar valueNei = (1 - weight) * flux;
            matrix.values[rowNeiStart + neiOffs[facei]] += value * operatorScalingNei;
            Kokkos::atomic_sub(
                &matrix.values[rowOwnStart + diagOffs[own]], value * operatorScalingOwn
            );

            // upper triangular part
            // add owner contribution lower
            value = flux * (1 - weight) * one<ValueType>();
            matrix.values[rowOwnStart + ownOffs[facei]] += value * operatorScalingOwn;
            Kokkos::atomic_sub(
                &matrix.values[rowNeiStart + diagOffs[nei]], value * operatorScalingNei
            );
        }
    );

    auto [refGradient, value, valueFraction, refValue] = views(
        phi.boundaryData().refGrad(),
        phi.boundaryData().value(),
        phi.boundaryData().valueFraction(),
        phi.boundaryData().refValue()
    );

    parallelFor(
        exec,
        {nInternalFaces, sFaceFlux.size()},
        KOKKOS_LAMBDA(const localIdx facei) {
            auto bcfacei = facei - nInternalFaces;
            scalar flux = sFaceFlux[facei];

            auto own = surfFaceCells[bcfacei];
            auto rowOwnStart = matrix.rowOffs[own];
            scalar operatorScalingOwn = operatorScaling[own];

            matrix.values[rowOwnStart + diagOffs[own]] +=
                flux * operatorScalingOwn * (1.0 - valueFraction[bcfacei]) * one<ValueType>();
            // TODO fix bc values
            rhs[own] -= (flux * operatorScalingOwn * (valueFraction[bcfacei] * refValue[bcfacei]));
            // + (1.0 - valueFraction[bcfacei]) * refGradient[bcfacei] / deltaCoeffs[facei]));
        }
    );
};

#define NF_DECLARE_COMPUTE_IMP_DIV(TYPENAME)                                                       \
    template void computeDivImp<                                                                   \
        TYPENAME>(la::LinearSystem<TYPENAME, localIdx>&, const SurfaceField<scalar>&, const VolumeField<TYPENAME>&, const dsl::Coeff, const SparsityPattern&)

NF_DECLARE_COMPUTE_IMP_DIV(scalar);
NF_DECLARE_COMPUTE_IMP_DIV(Vec3);

};
