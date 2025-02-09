// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2023 NeoFOAM authors
#pragma once

#include <cstdint>

#include "NeoFOAM/core/primitives/traits.hpp"


namespace NeoFOAM
{
#ifdef NEOFOAM_DP_LABEL
using label = int64_t;
using localIdx = uint64_t;
#else
using label = int32_t;
using localIdx = uint32_t;
#endif
using globalIdx = uint64_t;
using size_t = std::size_t;
using mpi_label_t = int;

// traits for vector
template<>
struct one<localIdx>
{
    static const inline localIdx value = 1;
};


template<>
struct zero<localIdx>
{
    static const inline localIdx value = 0;
};

}
