// Copyright (c) 2009-2016 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Developers
// Copyright (c) 2014-2016 The Dash Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/*
 * Argon2 source code package
 *
 * Written by Daniel Dinu and Dmitry Khovratovich, 2015
 *
 * This work is licensed under a Creative Commons CC0 1.0 License/Waiver.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with
 * this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

#ifndef ARGON2_OPT_H
#define ARGON2_OPT_H

#include "core.h"
#include <emmintrin.h>

/*
 * Function fills a new memory block. Differs from the
 * @param state Pointer to the just produced block. Content will be updated(!)
 * @param ref_block Pointer to the reference block
 * @param next_block Pointer to the block to be constructed
 * @pre all block pointers must be valid
 */
void fill_block(__m128i *state, const uint8_t *ref_block, uint8_t *next_block);

/*
 * Generate pseudo-random values to reference blocks in the segment and puts
 * them into the array
 * @param instance Pointer to the current instance
 * @param position Pointer to the current position
 * @param pseudo_rands Pointer to the array of 64-bit values
 * @pre pseudo_rands must point to @a instance->segment_length allocated values
 */
void generate_addresses(const argon2_instance_t *instance,
                        const argon2_position_t *position,
                        uint64_t *pseudo_rands);

#endif /* ARGON2_OPT_H */
