/**
 * Copyright (c) 2015 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * External documents:
 *  - "ARA_ES3_APBridge_RegisterMap_rev050.pdf
 *  - "ARA_ES3_APBridge_AppendixD_rev010.pdf
 *  - "ARA_ES3_APBridge_RegisterMap_AppendixD_rev001.pdf
 */
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include "chip.h"
#include "chipapi.h"
#include "debug.h"
#include "tsb_scm.h"
#include "tsb_isaa.h"
#include "common.h"
#include "bootrom.h"
#include "error.h"
#include "unipro.h"
#include "efuse.h"
#include "crypto.h"


/* Mask values used by "count_ones" */
#define MASK8_1S    ((uint8_t)0x55) /* binary: 0101... */
#define MASK8_2S    ((uint8_t)0x33) /* binary: 00110011.. */
#define MASK8_4S    ((uint8_t)0x0f) /* binary:  4 zeros,  4 ones ... */

#define MASK32_1S   ((uint32_t)0x55555555) /* binary: 0101... */
#define MASK32_2S   ((uint32_t)0x33333333) /* binary: 00110011.. */
#define MASK32_4S   ((uint32_t)0x0f0f0f0f) /* binary:  4 zeros,  4 ones ... */
#define MASK32_8S   ((uint32_t)0x00ff00ff) /* binary:  8 zeros,  8 ones ... */
#define MASK32_16S  ((uint32_t)0x0000ffff) /* binary:  16 zeros, 16 ones ... */


union large_uint {
  struct {
    uint32_t low;
    uint32_t high;
  };
  uint64_t quad;
};

static uint8_t          ims_value[TSB_ISAA_NUM_IMS_BYTES];

/* IMS is 35 bytes long, but boot ROM only cares about the first 32 bytes */
#define IMS_MEANINGFUL_LENGTH  32

/* Prototypes */
static int count_ones(uint8_t *buf, int len);
static bool valid_hamming_weight(uint8_t *buf, int len);
static bool is_buf_const(uint8_t *buf, uint32_t size, uint8_t val);
static bool get_endpoint_id(union large_uint * endpoint_id);


/**
 * @brief Valdate and publish the e-Fuses as DME attributes
 *
 * @param none
 *
 * @returns 0 on success
 *          -1 on failure
 */
int efuse_init(void) {
    uint32_t    register_val;
    uint32_t    urc;
    union large_uint endpoint_id;


    /* Check for e­-Fuse CRC error
     * See ARA_ESx_APBridge_RegisterMap_revxxx.pdf
     * TA-04 Read eFuse status (result ECC)
     */
    register_val = tsb_get_eccerror();
    if ((register_val & TSB_ECCERROR_ECC_ERROR) != 0) {
        dbgprint("efuse_init: Efuse ECC error\n");
        set_last_error(BRE_EFUSE_ECC);
        return -1;
    }


    /* Obtain and verify VID/PID/SN (in e­-Fuse) have proper Hamming weight
     * and advertise various these via DME attribute registers
     * These have 2 valid values:
     *      Unset:  0
     *      Set:    Must have equal number of 1's and 0's
     *
     * NB. The UniPro Mfgr's ID and PID are hard-wired into their DME
     * attributes, so there is no need to fetch/store them in this function.
     * However, ARA VID and PID are required by load_tftf_header(), and while
     * hardwired on real hardware, are not on the HAPS-62 or simulator. Rather
     * than write to read-only registers, so they can be retrieved in
     * load_tftf_header(), we validate them and cache them in the communication
     * area at the top of memory
     *
     * TA-13 Write/Read  DME attribute (New area of 16 words)
     * TA-03 Set e-Fuse data as SN, PID, VID, CMS, SCR, IMS and read...
     */
    ara_vid = tsb_get_vid();
    if (!valid_hamming_weight((uint8_t *)&ara_vid, sizeof(ara_vid))) {
        dbgprintx32("efuse_init: Invalid Ara VID: ", ara_vid, "\n");
        set_last_error(BRE_EFUSE_BAD_ARA_VID);
        return -1;
    }

    ara_pid = tsb_get_pid();
    if (!valid_hamming_weight((uint8_t *)&ara_pid, sizeof(ara_pid))) {
        dbgprintx32("efuse_init: Invalid Ara PID: ", ara_pid, "\n");
        set_last_error(BRE_EFUSE_BAD_ARA_PID);
        return -1;
    }

    /* Extract Internal Master Secret (IMS) from e­-Fuse, and if it is
     * non-zero, compute the Endpoint Unique ID
     */
    if (!get_endpoint_id(&endpoint_id)) {
        /*
         * Note that we get false returned if there was a bad IMS or if there
         * was no IMS from which to calculate a Unique Endpoint ID. Since
         * get_endpoint_id sets last error if it was a bad IMS, we use that
         * to differentiate between a benign omission and an error.
         */
        if (get_last_error() != BRE_OK) {
            return -1;
        }
    } else {
        dbgprintx64("efuse_init: endpoint ID: ", endpoint_id.quad, "\n");
        urc = chip_unipro_attr_write(DME_DDBL2_ENDPOINTID_L, endpoint_id.low, 0,
                                ATTR_LOCAL);
        if (urc) {
            set_last_error(BRE_EFUSE_ENDPOINT_ID_WRITE);
            return -1;
        }
        urc = chip_unipro_attr_write(DME_DDBL2_ENDPOINTID_H, endpoint_id.high,
                                0, ATTR_LOCAL);
        if (urc) {
            set_last_error(BRE_EFUSE_ENDPOINT_ID_WRITE);
            return -1;
        }
    }

    dbgprint("efuse_init: OK\n");
    return 0;
}


void efuse_rig_for_untrusted(void) {
    /* TA-21 Lock function with register (IMS, CMS) */
    tsb_disable_ims_access();
    tsb_disable_cms_access();
}


/**
 * @brief Count the number of "1" bits in an n-byte buffer
 *
 * @param buf The buffer to test
 * @param len The length in bytes of buf
 *
 * @returns The number of set bits in x (0 <= n <= 32)
 */
static int count_ones(uint8_t *buf, int len)
{
    uint8_t     x;
    uint32_t    x32;
    uint32_t *  buf32 = (uint32_t *) buf;
    int         count = 0;

    /* Process in 4-byte chunks as far as possible */
    while (len >= 4) {
        x32 = *buf32++;

        /* put count of each 2 bits into those 2 bits */
        x32 = (x32 & MASK32_1S) + ((x32 >> 1) & MASK32_1S);

        /* put count of each 4 bits into those 4 bits */
        x32 = (x32 & MASK32_2S) + ((x32 >> 2) & MASK32_2S);

        /* put count of each 8 bits into those 8 bits */
        x32 = (x32 & MASK32_4S) + ((x32 >> 4) & MASK32_4S);

        /* put count of each 16 bits into those 16 bits */
        x32 = (x32 & MASK32_8S) + ((x32 >> 8) & MASK32_8S);

        /* put count of each 16 bits into those 16 bits */
        x32 = (x32 + (x32 >> 16)) & MASK32_16S;

        count += x32;
        len -= 4;
    }

    /* Process any remaining bytes one at a time */
    buf = (uint8_t *)buf32;
    while (len > 0) {
        x = *buf++;

        /* put count of each 2 bits into those 2 bits */
        x -= (x >> 1) & MASK8_1S;

        /* put count of each 4 bits into those 4 bits */
        x = (x & MASK8_2S) + ((x >> 2) & MASK8_2S);

        /* put count of each 8 bits into those 8 bits */
        x = (x + (x >> 4)) & MASK8_4S;

        count += x;
        len--;
    }

    return count;
}


/**
 * @brief Verify that the buffer has the proper Hamming weight
 *
 * As specified in the high-level design, we validate certain e-Fuse
 * fields by their Hamming weight, for which there are 2 valid values:
 *      Unset:  0
 *      Set:    Equal numbers of 1's and 0's
 *
 * @param buf The buffer to test
 * @param len The length in bytes of buf
 *
 * @returns The number of set bits in x (0 <= n <= 32)
 */
static bool valid_hamming_weight(uint8_t *buf, int len)
{
    int         count;

    count = count_ones(buf, len);

    return ((count == 0) ||
            (count == (len * 8 / 2)));
}


/**
 * @brief Determine if a buffer is filled with a constant value
 *
 * @param buf The buffer to test.
 * @param size The size in bytes of buf,
 * @param val The constant value to check against.
 *
 * @returns Nothing
 */
static bool is_buf_const(uint8_t *buf, uint32_t size, uint8_t val) {
    uint32_t val32 = val;
    val32 |= (val32 << 8);
    val32 |= (val32 << 16);
    while (size > sizeof(uint32_t)) {
        if (*(uint32_t *)buf != val32) {
            return false;
        }
        buf += sizeof(uint32_t);
        size -= sizeof(uint32_t);
    }

    while (size > 0) {
        if (*buf++ != val) {
            return false;
        }
        size--;
    }

    return true;
}


/**
 * @brief Extract Internal Master Secret (IMS) from e­-Fuse
 *
 * If the IMS is non-zero, compute the Endpoint Unique ID from it.
 *
 * @param endpoint_id Pointer to a 64-bit field, into which will go the
 * calculated endpoint ID
 * @returns True if it calculated an endpoint ID, false if there was no IMS
 * from which to calculate it (get_last_error will return BRE_OK), or if
 * the IMS was deemed invalid (get_last_error will return BRE_EFUSE_BAD_IMS).
 */
static bool get_endpoint_id(union large_uint * endpoint_id) {
    /* Establish the default (i.e., no endpoint ID) */
    bool have_endpoint_id = false;
    endpoint_id->quad = 0;

    /* Get the IMS and determine the course of action if non-zero */
    tsb_get_ims(ims_value, IMS_MEANINGFUL_LENGTH);
    if (!is_buf_const(ims_value, IMS_MEANINGFUL_LENGTH, 0)) {
        /* Compute Endpoint Unique ID */
        if (!valid_hamming_weight((uint8_t *)ims_value,
                                  IMS_MEANINGFUL_LENGTH)) {
            dbgprint("efuse_init: Invalid IMS\n");
            set_last_error(BRE_EFUSE_BAD_IMS);
        } else {
#ifdef _SIMULATION
            /* some fake value for simulation build */
            endpoint_id->low = 0x12345678;
            endpoint_id->high = 0x9ABCDEF0;
#else
            int i;
            /**
             * The algorithm used to calculate Endpoint Unique ID is:
             * Y1 = sha256(IMS[0:15] xor copy(0x3d, 16))
             * Z0 = sha256(Y1 || copy(0x01, 32))
             * EP_UID[0:7] = sha256(Z0)[0:7]
             */
            unsigned char EP_UID[HASH_DIGEST_SIZE];
            unsigned char Y1[HASH_DIGEST_SIZE];
            unsigned char Z0[HASH_DIGEST_SIZE];
            uint32_t temp;
            uint32_t *pims = (uint32_t *)ims_value;

            hash_start();
            /*** grab IMS 4bytes at a time and feed that to hash_update */
            for (i = 0; i < 4; i++) {
                temp = pims[i] ^ 0x3d3d3d3d;
                hash_update((unsigned char *)&temp, 1);
            }
            hash_final(Y1);

            hash_start();
            hash_update(Y1, HASH_DIGEST_SIZE);
            temp = 0x01010101;
            for (i = 0; i < 8; i++) {;
                hash_update((unsigned char *)&temp, 1);
            }
            hash_final(Z0);

            hash_start();
            hash_update(Z0, HASH_DIGEST_SIZE);
            hash_final(EP_UID);

            memcpy(endpoint_id, EP_UID, 8);
#endif
            have_endpoint_id =  true;
        }
    }

    return have_endpoint_id;
}
