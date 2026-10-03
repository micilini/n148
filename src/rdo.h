/*
 * N.148i rate-distortion optimization helpers.
 *
 * Copyright (c) 2026 Micilini Roll
 * Licensed under the MIT License. See LICENSE in the project root.
 */

#ifndef N148_RDO_H
#define N148_RDO_H

#include <stdint.h>

typedef struct {
    uint32_t input_nonzeros;
    uint32_t output_nonzeros;
    uint32_t changed_coefficients;
    double input_cost;
    double output_cost;
} N148RdoResult;

/* Estimate the coefficient portion of the existing rANS syntax. The estimate
   includes amplitude bits and inexpensive proxies for AC symbols and EOB. */
double n148_rdo_estimate_block_bits(const short coefficients[64]);

/* Return the proxy minimized by the trellis for AC coefficients. */
double n148_rdo_proxy_cost(const float ideal_levels[64],
                           const double unit_energy[64],
                           const short coefficients[64], double lambda);

/* Refine one quantized block. DC remains fixed because changing it affects
   causal prediction much more strongly than the separable AC proxy models. */
int n148_rdo_refine_coefficients(const float ideal_levels[64],
                                 const double unit_energy[64],
                                 double lambda, int effort,
                                 short coefficients[64],
                                 N148RdoResult *result);

/* Internal encoder path for freshly computed, finite transform data. */
int n148_rdo_refine_coefficients_trusted(const float ideal_levels[64],
                                         const double unit_energy[64],
                                         double lambda, int effort,
                                         short coefficients[64],
                                         N148RdoResult *result);

#endif
