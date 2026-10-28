/*
 * SAE (Simultaneous Authentication of Equals) for WPA3-Personal
 * Based on IEEE 802.11-2020 Section 12.4 and RFC 7664
 *
 * Copyright (c) 2026 Marc Davis
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SAE_H
#define SAE_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SAE Group 19: NIST P-256 (secp256r1) — mandatory for WPA3 */
#define SAE_GROUP_ID		19
#define SAE_EC_ORDER_LEN	32	/* 256 bits */
#define SAE_PRIME_LEN		32
#define SAE_POINT_LEN		64	/* uncompressed x,y */
#define SAE_PMK_LEN		32
#define SAE_PMKID_LEN		16
#define SAE_KCK_LEN		32
#define SAE_MAX_PASSWORD_LEN	128
#define SAE_MAX_TOKEN_LEN	256

/* Auth algorithm number for SAE (IEEE 802.11-2020 Table 9-57) */
#define WLAN_AUTH_SAE		3

/* SAE status codes */
#define SAE_STATUS_SUCCESS			0
#define SAE_STATUS_UNKNOWN_PASSWORD_IDENTIFIER	123
#define SAE_STATUS_HASH_TO_ELEMENT		126

/* SAE auth transaction numbers */
#define SAE_COMMIT	1
#define SAE_CONFIRM	2

/* SAE states */
enum sae_state {
	SAE_STATE_NOTHING = 0,
	SAE_STATE_COMMITTED,
	SAE_STATE_CONFIRMED,
	SAE_STATE_ACCEPTED,
};

/* SAE context — one per authentication attempt */
struct sae_data {
	enum sae_state state;
	uint16_t group;
	uint16_t sync;		/* retransmission counter */
	uint16_t send_confirm;	/* confirm counter */

	/* Our private random: rand ∈ [2, q-1] */
	uint8_t rand[SAE_EC_ORDER_LEN];

	/* Our scalar = (rand + mask) mod q */
	uint8_t scalar[SAE_EC_ORDER_LEN];

	/* Our element = inverse(mask * PWE) */
	uint8_t element[SAE_POINT_LEN];

	/* Peer's scalar and element */
	uint8_t peer_scalar[SAE_EC_ORDER_LEN];
	uint8_t peer_element[SAE_POINT_LEN];

	/* Shared secret (x-coordinate of shared point) */
	uint8_t shared_secret[SAE_PRIME_LEN];

	/* Derived keys */
	uint8_t kck[SAE_KCK_LEN];	/* Key Confirmation Key */
	uint8_t pmk[SAE_PMK_LEN];	/* Pairwise Master Key */
	uint8_t pmkid[SAE_PMKID_LEN];

	/* Password Element — the shared point on the curve */
	uint8_t pwe[SAE_POINT_LEN];

	/* Anti-clogging token from AP (if any) */
	uint8_t token[SAE_MAX_TOKEN_LEN];
	uint16_t token_len;

	/* Our and peer MAC addresses */
	uint8_t own_addr[6];
	uint8_t peer_addr[6];

	/* Confirm values */
	uint8_t own_confirm[32];
	uint8_t peer_confirm[32];
};

/*
 * Initialize SAE context for a new authentication.
 * password/password_len: the network passphrase
 * own_addr/peer_addr: 6-byte MAC addresses
 * Returns 0 on success.
 */
int sae_prepare_commit(struct sae_data *sae,
		       const uint8_t *password, size_t password_len,
		       const uint8_t *own_addr, const uint8_t *peer_addr);

/*
 * Build the SAE Commit frame body (after the auth header).
 * buf must have room for at least 2 + 2 + 32 + 64 = 100 bytes.
 * Returns the number of bytes written, or -1 on error.
 */
int sae_write_commit(const struct sae_data *sae,
		     uint8_t *buf, size_t buflen);

/*
 * Parse a received SAE Commit frame body.
 * Returns 0 on success, -1 on parse error.
 */
int sae_parse_commit(struct sae_data *sae,
		     const uint8_t *buf, size_t len);

/*
 * Process the peer's commit and derive shared secret + keys.
 * Must be called after sae_parse_commit().
 * Returns 0 on success.
 */
int sae_process_commit(struct sae_data *sae);

/*
 * Build the SAE Confirm frame body.
 * buf must have room for at least 2 + 32 = 34 bytes.
 * Returns the number of bytes written, or -1 on error.
 */
int sae_write_confirm(struct sae_data *sae,
		      uint8_t *buf, size_t buflen);

/*
 * Parse and verify a received SAE Confirm frame body.
 * Returns 0 on success (confirm matches), -1 on failure.
 */
int sae_check_confirm(struct sae_data *sae,
		      const uint8_t *buf, size_t len);

/*
 * Reset/free SAE context.
 */
void sae_clear(struct sae_data *sae);

#ifdef __cplusplus
}
#endif

#endif /* SAE_H */
