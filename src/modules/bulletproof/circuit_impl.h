/**********************************************************************
 * Copyright (c) 2018 Andrew Poelstra                                 *
 * Distributed under the MIT software license, see the accompanying   *
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.*
 **********************************************************************/

#ifndef SECP256K1_MODULE_BULLETPROOF_CIRCUIT_IMPL
#define SECP256K1_MODULE_BULLETPROOF_CIRCUIT_IMPL

#include "modules/bulletproof/inner_product_impl.h"
#include "modules/bulletproof/util.h"
#include "group.h"

#include <stdlib.h>

typedef struct {
    secp256k1_rfc6979_hmac_sha256 rng;
    secp256k1_scalar y;
    secp256k1_scalar yinv;
    secp256k1_scalar z;
    secp256k1_scalar yn;
    secp256k1_scalar yinvn;
    secp256k1_scalar sl, sr;
    const secp256k1_bulletproof_circuit_assignment *assn;
    const secp256k1_bulletproof_pf_compressed_circuit *comp_circ;
    size_t i;
} secp256k1_bulletproof_circuit_lr_generator;

static void secp256k1_bulletproof_circuit_lr_generator_update(secp256k1_bulletproof_circuit_lr_generator *generator);

static void secp256k1_bulletproof_circuit_lr_generator_init(secp256k1_bulletproof_circuit_lr_generator *generator, const secp256k1_rfc6979_hmac_sha256 *rng, const secp256k1_scalar *y, const secp256k1_scalar *z) {
    memcpy(&generator->rng, rng, sizeof(*rng));
    generator->y = *y;
    generator->z = *z;
    secp256k1_scalar_set_int(&generator->yn, 1);
    secp256k1_scalar_set_int(&generator->yinvn, 1);
    secp256k1_scalar_inverse_var(&generator->yinv, y);
    generator->i = 0;
    secp256k1_bulletproof_genrand_pair(&generator->rng, &generator->sl, &generator->sr);
}

static void secp256k1_bulletproof_circuit_lr_generator_finalize(secp256k1_bulletproof_circuit_lr_generator *generator) {
    secp256k1_rfc6979_hmac_sha256_finalize(&generator->rng);
}

static void secp256k1_bulletproof_circuit_lr_generator_update(secp256k1_bulletproof_circuit_lr_generator *generator) {
    secp256k1_bulletproof_genrand_pair(&generator->rng, &generator->sl, &generator->sr);

    secp256k1_scalar_mul(&generator->yn, &generator->yn, &generator->y);
    secp256k1_scalar_mul(&generator->yinvn, &generator->yinvn, &generator->yinv);
    generator->i += 1;
}

static void secp256k1_bulletproof_circuit_lr_generate(const secp256k1_bulletproof_circuit_lr_generator *generator, secp256k1_scalar *lout, secp256k1_scalar *rout, const secp256k1_scalar *x) {
    secp256k1_scalar negone;
    secp256k1_scalar sl, sr;
    secp256k1_scalar x2, x3;
    secp256k1_scalar tmp;
    secp256k1_scalar al, ar, ao;
    const size_t i = generator->i;

    if (i < generator->assn->n_gates) {
        al = generator->assn->al[i];
        ar = generator->assn->ar[i];
        ao = generator->assn->ao[i];
    } else {
        secp256k1_scalar_clear(&al);
        secp256k1_scalar_clear(&ar);
        secp256k1_scalar_clear(&ao);
    }

    secp256k1_scalar_set_int(&negone, 1);
    secp256k1_scalar_negate(&negone, &negone);

    secp256k1_scalar_sqr(&x2, x);
    secp256k1_scalar_mul(&x3, &x2, x);

    secp256k1_scalar_mul(&sl, &generator->sl, &x3);
    secp256k1_scalar_mul(&sr, &generator->sr, &x3);
    secp256k1_scalar_mul(&sr, &sr, &generator->yn);
    secp256k1_scalar_mul(lout, &ao, x); /* l = a_O * x */
    secp256k1_scalar_add(lout, lout, &al); /* l = a_O * x + a_L */
    secp256k1_scalar_mul(rout, &ar, x); /* r = a_R * X */
    secp256k1_scalar_add(rout, rout, &negone); /* r = a_R * X - 1 */
    secp256k1_scalar_mul(rout, rout, &generator->yn); /* r = y^n * a_R * x - y^n */

    secp256k1_scalar_add(lout, lout, &generator->comp_circ->wr[i]);
    /* ^  l = a_O * x + a_L + y^-n (z^Q . W_R)  */

    secp256k1_scalar_mul(&tmp, &generator->comp_circ->wl[i], x);
    secp256k1_scalar_add(rout, rout, &tmp);

    secp256k1_scalar_add(rout, rout, &generator->comp_circ->wo[i]);
    /* ^  r = y^n * a_R * x - y^n + z^Q . (xW_L + W_O) */

    secp256k1_scalar_mul(lout, lout, x); /* l = a_O * x^2 + (a_L + y^-n (z^Q . W_R)) * x  */

    secp256k1_scalar_add(lout, lout, &sl); /* add s_L * x^3 */
    secp256k1_scalar_add(rout, rout, &sr); /* add s_R * x^3 */
}

typedef struct {
    secp256k1_scalar x;
    secp256k1_scalar cache;
    secp256k1_bulletproof_circuit_lr_generator lr_gen;
} secp256k1_bulletproof_circuit_abgh_data;

static int secp256k1_bulletproof_circuit_abgh_callback(secp256k1_scalar *sc, secp256k1_ge *pt, size_t idx, void *data) {
    secp256k1_bulletproof_circuit_abgh_data *ctx = (secp256k1_bulletproof_circuit_abgh_data *) data;
    const int is_g = idx % 2 == 0;

    (void) pt;
    if (is_g) {
        secp256k1_bulletproof_circuit_lr_generate(&ctx->lr_gen, sc, &ctx->cache, &ctx->x);
        secp256k1_bulletproof_circuit_lr_generator_update(&ctx->lr_gen);
    } else {
        *sc = ctx->cache;
    }

    return 1;
}

/* Proof format:
 *
 * Serialized scalars (32 bytes) t, tau_x, mu, a, b
 * Serialized points (bit array of parity followed by 32 bytes): A_I, A_O, S, T_1, T_3, T_4, T_5, T_6, [inner product proof points]
 */
static int secp256k1_bulletproof_relation66_prove_impl(const secp256k1_ecmult_context *ecmult_ctx, secp256k1_scratch *scratch, unsigned char *proof, size_t *plen, const secp256k1_bulletproof_circuit_assignment *assn, const secp256k1_ge *commitp, const secp256k1_scalar *blinds, size_t nc, const secp256k1_ge *genp, const secp256k1_bulletproof_circuit *circ, const secp256k1_ge *gens, const size_t n_gens, const unsigned char *nonce, const unsigned char *extra_commit, size_t extra_commit_len) {
    secp256k1_bulletproof_circuit_lr_generator lr_gen;
    secp256k1_bulletproof_circuit_abgh_data abgh_data;
    secp256k1_rfc6979_hmac_sha256 rng;
    secp256k1_sha256 sha256;
    unsigned char commit[32] = {0};
    secp256k1_scalar zero, one, onehalf, onethird, twothirds, fourthirds, eightthirds;
    secp256k1_scalar alpha, beta, rho, mu;
    secp256k1_scalar tau1, tau3, tau4, tau5, tau6, taux; /* tau2 missing on purpose */
    secp256k1_scalar t[7];  /* t[1..6] are coefficients; t[0] is the polynomial evaluated at x */
    secp256k1_scalar tauv;  /* <z, WV*gamma> term in eq (73) */
    secp256k1_scalar x, xn, y, yinv, z;
    secp256k1_scalar tmp;
    secp256k1_gej aij, aoj, sj;
    secp256k1_ge tmpge;
    secp256k1_ge out_pt[8];
    int overflow;
    size_t i;

    if (assn->n_gates > circ->n_gates || assn->n_commits > circ->n_commits || nc != circ->n_commits) {
        return 0;
    }
    if (*plen < 64 + 256 + 1) {  /* inner product argument will do a more precise check and assignment */
        return 0;
    }

    /* Commit to all input data */
    secp256k1_bulletproof_update_commit_n(commit, commitp, nc);
    secp256k1_bulletproof_update_commit_n(commit, genp, 1);
    /* TODO commit to circuit */
    if (extra_commit != NULL) {
        secp256k1_sha256_initialize(&sha256);
        secp256k1_sha256_write(&sha256, commit, 32);
        secp256k1_sha256_write(&sha256, extra_commit, extra_commit_len);
        secp256k1_sha256_finalize(&sha256, commit);
    }

    /* Setup, generate randomness */
    secp256k1_scalar_set_int(&zero, 0);
    secp256k1_scalar_set_int(&one, 1);
    secp256k1_scalar_set_int(&tmp, 6);
    secp256k1_scalar_inverse_var(&tmp, &tmp);
    secp256k1_scalar_set_int(&onethird, 2);
    secp256k1_scalar_mul(&onethird, &onethird, &tmp);
    secp256k1_scalar_set_int(&onehalf, 3);
    secp256k1_scalar_mul(&onehalf, &onehalf, &tmp);
    secp256k1_scalar_add(&twothirds, &onethird, &onethird);
    secp256k1_scalar_add(&fourthirds, &twothirds, &twothirds);
    secp256k1_scalar_add(&eightthirds, &fourthirds, &fourthirds);

    secp256k1_rfc6979_hmac_sha256_initialize(&rng, nonce, 32); /* todo initialize from secret input */
    secp256k1_bulletproof_genrand_pair(&rng, &alpha, &beta);
    secp256k1_bulletproof_genrand_pair(&rng, &rho, &tau1);
    secp256k1_bulletproof_genrand_pair(&rng, &tau3, &tau4); /* t2 will be generated deterministically */
    secp256k1_bulletproof_genrand_pair(&rng, &tau5, &tau6);

    /* Compute A_I, A_O, S */
    lr_gen.rng = rng;

    secp256k1_ecmult_const(&aij, &secp256k1_ge_const_g, &alpha, 256);
    for (i = 0; i < circ->n_bits; i++) {
        secp256k1_ge aterm = gens[i + n_gens/2];

        secp256k1_ge_neg(&aterm, &aterm);
        secp256k1_fe_cmov(&aterm.x, &gens[i].x, secp256k1_scalar_is_one(&assn->al[i]));
        secp256k1_fe_cmov(&aterm.y, &gens[i].y, secp256k1_scalar_is_one(&assn->al[i]));
        secp256k1_gej_add_ge(&aij, &aij, &aterm);
    }
    secp256k1_ge_set_gej(&tmpge, &aij);
    secp256k1_bulletproof_vector_commit(&aij, assn->al + circ->n_bits, gens + circ->n_bits, assn->n_gates - circ->n_bits, NULL, NULL);
    secp256k1_gej_add_ge(&aij, &aij, &tmpge);
    secp256k1_ge_set_gej(&tmpge, &aij);
    secp256k1_bulletproof_vector_commit(&aij, assn->ar + circ->n_bits, gens + circ->n_bits + n_gens/2, assn->n_gates - circ->n_bits, NULL, NULL);
    secp256k1_gej_add_ge(&aij, &aij, &tmpge);

    secp256k1_bulletproof_vector_commit(&aoj, assn->ao + circ->n_bits, gens + circ->n_bits, assn->n_gates - circ->n_bits, &beta, NULL);

    secp256k1_ecmult_const(&sj, &secp256k1_ge_const_g, &rho, 256);
    for (i = 0; i < circ->n_gates; i++) {
        secp256k1_scalar sl, sr;
        secp256k1_gej termj;
        secp256k1_ge term;

        secp256k1_bulletproof_genrand_pair(&lr_gen.rng, &sl, &sr);

        secp256k1_ecmult_const(&termj, &gens[i], &sl, 256);
        secp256k1_ge_set_gej(&term, &termj);
        secp256k1_gej_add_ge(&sj, &sj, &term);
        secp256k1_ecmult_const(&termj, &gens[i + n_gens/2], &sr, 256);
        secp256k1_ge_set_gej(&term, &termj);
        secp256k1_gej_add_ge(&sj, &sj, &term);
    }
    secp256k1_rfc6979_hmac_sha256_finalize(&lr_gen.rng);

    /* get challenges y and z */
    secp256k1_ge_set_gej(&out_pt[0], &aij);
    secp256k1_ge_set_gej(&out_pt[1], &aoj);
    secp256k1_ge_set_gej(&out_pt[2], &sj);

    secp256k1_bulletproof_update_commit_n(commit, &out_pt[0], 3);
    secp256k1_scalar_set_b32(&y, commit, &overflow);
    if (overflow || secp256k1_scalar_is_zero(&y)) {
        return 0;
    }
    secp256k1_bulletproof_update_commit_n(commit, NULL, 0);
    secp256k1_scalar_set_b32(&z, commit, &overflow);
    if (overflow || secp256k1_scalar_is_zero(&z)) {
        return 0;
    }

    secp256k1_scalar_inverse_var(&yinv, &y);

    if (!secp256k1_bulletproof_pf_compressed_circuit_allocate_frame(scratch, circ)) {
        return 0;
    }

    lr_gen.assn = assn;
    lr_gen.comp_circ = secp256k1_bulletproof_pf_compress_circuit(scratch, circ, &y, &yinv, &z);

    /* Compute coefficients t[1..6] */

    /* Start by computing each entry of l and r, as
     *   l = l1 * X          + l2 * X^2 + l3 * X^3
     *   r = r0     + r1 * X            + r3 * X^3
     * and observe that
     *   t1 = <l1, r0>
     *   t2 = <l1, r1> + <l2, r0>
     *   t3 = <l2, r1> + <l3, r0>
     *   t4 = <l3, r1> + <l1, r3>
     *   t5 = <l2, r3>
     *   t6 = <l3, r3>
     * So we compute these terms and add them to t1,t3,etc as running sums.
     */

    for (i = 0; i < 6; i++) {
        secp256k1_scalar_clear(&t[i + 1]);
    }
    secp256k1_bulletproof_circuit_lr_generator_init(&lr_gen, &rng, &y, &z);
    for (i = 0; i < circ->n_gates; i++) {
        secp256k1_scalar lone, rone;
        secp256k1_scalar lhalf, rhalf;
        secp256k1_scalar ltmp, rtmp;
        secp256k1_scalar l1, l3;     /* l coefficients -- l2 = a_O[i], l0 = 0 */
        secp256k1_scalar r0, r1, r3; /* r coefficients -- r2 = 0 */
        secp256k1_scalar ao;

        if (i < assn->n_gates) {
            ao = assn->ao[i];
        } else {
            secp256k1_scalar_clear(&ao);
        }

        secp256k1_bulletproof_circuit_lr_generate(&lr_gen, &lone, &r0, &zero);
        secp256k1_bulletproof_circuit_lr_generate(&lr_gen, &lone, &rone, &one);
        secp256k1_bulletproof_circuit_lr_generate(&lr_gen, &lhalf, &rhalf, &onehalf);
        secp256k1_bulletproof_circuit_lr_generator_update(&lr_gen);

        secp256k1_scalar_add(&l1, &lone, &ao); /* l1 = l(1) + l2 + l0 */
        secp256k1_scalar_add(&r1, &rone, &r0);
        secp256k1_scalar_mul(&l1, &l1, &onethird); /* l1 = 1/3 l(1) + 1/3 l2 + 1/3 l0 */
        secp256k1_scalar_mul(&r1, &r1, &onethird);
        secp256k1_scalar_add(&r1, &r1, &r0);
        secp256k1_scalar_add(&r1, &r1, &r0); /* l1 = 1/3 l(1) + 1/3 l2 + 7/3 l0 */
        secp256k1_scalar_negate(&l1, &l1); /* l1 = -1/3 l(1) - 1/3 l2 - 7/3 l0 */
        secp256k1_scalar_negate(&r1, &r1);

        secp256k1_scalar_mul(&ltmp, &lhalf, &eightthirds);
        secp256k1_scalar_mul(&rtmp, &rhalf, &eightthirds);
        secp256k1_scalar_add(&l1, &l1, &ltmp); /* l1 = -1/3 l(1) + 8/3 l(1/2) - 1/3 l2 - 7/3 l0 */
        secp256k1_scalar_add(&r1, &r1, &rtmp);

        secp256k1_scalar_mul(&l3, &ao, &twothirds); /* l3 = 2/3 l2 */
        secp256k1_scalar_add(&l3, &l3, &ltmp); /* l3 = 2/3 l2 + 8/3 l(1/2) */
        secp256k1_scalar_negate(&l3, &l3); /* l3 = -2/3 l2 - 8/3 l(1/2) */
        secp256k1_scalar_negate(&r3, &rtmp);

        secp256k1_scalar_mul(&ltmp, &lone, &fourthirds);
        secp256k1_scalar_add(&rtmp, &r0, &rone);
        secp256k1_scalar_mul(&rtmp, &rtmp, &fourthirds);
        secp256k1_scalar_add(&l3, &l3, &ltmp); /* l3 = -2/3 l2 - 8/3 l(1/2) + 4/3 l(1) + 4/3 l0 */
        secp256k1_scalar_add(&r3, &r3, &rtmp);

        /* Now that we have the individual coefficients, compute the dot product */
        secp256k1_scalar_mul(&ltmp, &l1, &r0);
        secp256k1_scalar_add(&t[1], &t[1], &ltmp);

        secp256k1_scalar_mul(&ltmp, &l1, &r1);
        secp256k1_scalar_add(&t[2], &t[2], &ltmp);
        secp256k1_scalar_mul(&ltmp, &ao, &r0);
        secp256k1_scalar_add(&t[2], &t[2], &ltmp);

        secp256k1_scalar_mul(&ltmp, &ao, &r1);
        secp256k1_scalar_add(&t[3], &t[3], &ltmp);
        secp256k1_scalar_mul(&ltmp, &l3, &r0);
        secp256k1_scalar_add(&t[3], &t[3], &ltmp);

        secp256k1_scalar_mul(&ltmp, &l3, &r1);
        secp256k1_scalar_add(&t[4], &t[4], &ltmp);
        secp256k1_scalar_mul(&ltmp, &l1, &r3);
        secp256k1_scalar_add(&t[4], &t[4], &ltmp);

        secp256k1_scalar_mul(&ltmp, &ao, &r3);
        secp256k1_scalar_add(&t[5], &t[5], &ltmp);

        secp256k1_scalar_mul(&ltmp, &l3, &r3);
        secp256k1_scalar_add(&t[6], &t[6], &ltmp);
    }
    secp256k1_bulletproof_circuit_lr_generator_finalize(&lr_gen);

    /* Compute T1, T3, T4, T5, T6 */
    secp256k1_bulletproof_vector_commit(&aij, &t[1], genp, 1, &tau1, NULL);
    secp256k1_ge_set_gej(&out_pt[3], &aij);

    secp256k1_bulletproof_vector_commit(&aij, &t[3], genp, 1, &tau3, NULL);
    secp256k1_ge_set_gej(&out_pt[4], &aij);

    secp256k1_bulletproof_vector_commit(&aij, &t[4], genp, 1, &tau4, NULL);
    secp256k1_ge_set_gej(&out_pt[5], &aij);

    secp256k1_bulletproof_vector_commit(&aij, &t[5], genp, 1, &tau5, NULL);
    secp256k1_ge_set_gej(&out_pt[6], &aij);

    secp256k1_bulletproof_vector_commit(&aij, &t[6], genp, 1, &tau6, NULL);
    secp256k1_ge_set_gej(&out_pt[7], &aij);

    /* Compute x, tau_x, mu and t */
    secp256k1_bulletproof_update_commit_n(commit, &out_pt[3], 5);
    secp256k1_scalar_set_b32(&x, commit, &overflow);
    if (overflow || secp256k1_scalar_is_zero(&x)) {
        secp256k1_scratch_deallocate_frame(scratch);
        return 0;
    }

    secp256k1_scalar_mul(&alpha, &alpha, &x);
    secp256k1_scalar_mul(&tau1, &tau1, &x);

    secp256k1_scalar_sqr(&xn, &x);
    secp256k1_scalar_mul(&beta, &beta, &xn);
    secp256k1_scalar_clear(&tauv);
    for (i = 0; i < circ->n_commits; i++) {
        secp256k1_scalar zwv;
        secp256k1_scalar_mul(&zwv, &lr_gen.comp_circ->wv[i], &blinds[i]);
        secp256k1_scalar_add(&tauv, &tauv, &zwv);
    }
    secp256k1_scalar_mul(&tauv, &tauv, &xn);

    secp256k1_scalar_mul(&xn, &xn, &x);
    secp256k1_scalar_mul(&rho, &rho, &xn);
    secp256k1_scalar_mul(&tau3, &tau3, &xn);

    secp256k1_scalar_mul(&xn, &xn, &x);
    secp256k1_scalar_mul(&tau4, &tau4, &xn);

    secp256k1_scalar_mul(&xn, &xn, &x);
    secp256k1_scalar_mul(&tau5, &tau5, &xn);

    secp256k1_scalar_mul(&xn, &xn, &x);
    secp256k1_scalar_mul(&tau6, &tau6, &xn);

    secp256k1_scalar_add(&taux, &tau1, &tauv);
    secp256k1_scalar_add(&taux, &taux, &tau3);
    secp256k1_scalar_add(&taux, &taux, &tau4);
    secp256k1_scalar_add(&taux, &taux, &tau5);
    secp256k1_scalar_add(&taux, &taux, &tau6);

#ifdef VERIFY
{
    secp256k1_scalar tcheck;

    secp256k1_scalar_clear(&t[0]);
    secp256k1_scalar_add(&t[0], &t[0], &t[6]);
    secp256k1_scalar_mul(&t[0], &t[0], &x);
    secp256k1_scalar_add(&t[0], &t[0], &t[5]);
    secp256k1_scalar_mul(&t[0], &t[0], &x);
    secp256k1_scalar_add(&t[0], &t[0], &t[4]);
    secp256k1_scalar_mul(&t[0], &t[0], &x);
    secp256k1_scalar_add(&t[0], &t[0], &t[3]);
    secp256k1_scalar_mul(&t[0], &t[0], &x);
    secp256k1_scalar_add(&t[0], &t[0], &t[2]);
    secp256k1_scalar_mul(&t[0], &t[0], &x);
    secp256k1_scalar_add(&t[0], &t[0], &t[1]);
    secp256k1_scalar_mul(&t[0], &t[0], &x);

    secp256k1_scalar_clear(&tcheck);
    secp256k1_bulletproof_circuit_lr_generator_init(&lr_gen, &rng, &y, &z);
    for (i = 0; i < circ->n_gates; i++) {
        secp256k1_scalar lx, rx;
        secp256k1_bulletproof_circuit_lr_generate(&lr_gen, &lx, &rx, &x);
        secp256k1_bulletproof_circuit_lr_generator_update(&lr_gen);
        secp256k1_scalar_mul(&lx, &lx, &rx);
        secp256k1_scalar_add(&tcheck, &tcheck, &lx);
    }
    secp256k1_bulletproof_circuit_lr_generator_finalize(&lr_gen);
    CHECK(secp256k1_scalar_eq(&t[0], &tcheck));
}
#endif

    secp256k1_scalar_add(&mu, &alpha, &beta);
    secp256k1_scalar_add(&mu, &mu, &rho);

    /* Negate taux and mu so verifier doesn't have to */
    secp256k1_scalar_negate(&mu, &mu);
    secp256k1_scalar_negate(&taux, &taux);

    /* Encode circuit stuff */
    secp256k1_scalar_get_b32(&proof[0], &taux);
    secp256k1_scalar_get_b32(&proof[32], &mu);
    secp256k1_bulletproof_serialize_points(&proof[64], out_pt, 8);

    /* Mix these scalars into the hash so the input to the inner product proof is fixed */
    secp256k1_sha256_initialize(&sha256);
    secp256k1_sha256_write(&sha256, commit, 32);
    secp256k1_sha256_write(&sha256, proof, 64);
    secp256k1_sha256_finalize(&sha256, commit);

    /* Compute l and r, do inner product proof */
    abgh_data.x = x;
    secp256k1_bulletproof_circuit_lr_generator_init(&abgh_data.lr_gen, &rng, &y, &z);
    abgh_data.lr_gen.assn = lr_gen.assn;
    abgh_data.lr_gen.comp_circ = lr_gen.comp_circ;
    *plen -= 64 + 256 + 1;
    if (secp256k1_bulletproof_inner_product_prove_impl(ecmult_ctx, scratch, &proof[64 + 256 + 1], plen, gens, n_gens, &yinv, circ->n_gates, secp256k1_bulletproof_circuit_abgh_callback, (void *) &abgh_data, commit) == 0) {
        secp256k1_scratch_deallocate_frame(scratch);
        return 0;
    }
    *plen += 64 + 256 + 1;
    secp256k1_bulletproof_circuit_lr_generator_finalize(&abgh_data.lr_gen);

    secp256k1_rfc6979_hmac_sha256_finalize(&rng);

    secp256k1_scratch_deallocate_frame(scratch);
    return 1;
}

typedef struct  {
    secp256k1_scalar x;
    secp256k1_scalar y;
    secp256k1_scalar yinv;
    secp256k1_scalar z;
    const secp256k1_bulletproof_vfy_compressed_circuit *comp_circ;
    /* state tracking */
    size_t count;
    /* eq 83 */
    secp256k1_ge age[3];
    /* eq 82 */
    secp256k1_scalar randomizer82;
    secp256k1_ge tge[5];
    secp256k1_scalar t;
    const secp256k1_ge *genp;
    const secp256k1_ge *commits;
    size_t n_gates;
    size_t n_commits;
} secp256k1_bulletproof_circuit_vfy_ecmult_context;

static int secp256k1_bulletproof_circuit_vfy_callback(secp256k1_scalar *sc, secp256k1_ge *pt, secp256k1_scalar *randomizer, size_t idx, void *data) {
    secp256k1_bulletproof_circuit_vfy_ecmult_context *ctx = (secp256k1_bulletproof_circuit_vfy_ecmult_context *) data;

    if (idx < ctx->n_gates) { /* Gi */
        secp256k1_scalar_mul(sc, &ctx->comp_circ->wr[idx], &ctx->x);
        secp256k1_scalar_mul(sc, sc, randomizer);
    } else if (idx < 2 * ctx->n_gates) { /* Hi */
        secp256k1_scalar dot;
        idx -= ctx->n_gates;

        secp256k1_scalar_set_int(&dot, 1);
        secp256k1_scalar_negate(&dot, &dot);
        secp256k1_scalar_add(sc, &ctx->comp_circ->wl_wo[idx], &dot);

        secp256k1_scalar_mul(sc, sc, randomizer);
    /* return a (scalar, point) pair to add to the multiexp */
    } else {
        switch(ctx->count) {
        /* g^(x^2(k + <z^Q, c>) - t) (82) */
        case 0: {
            secp256k1_scalar_negate(sc, &ctx->t);
            secp256k1_scalar_add(sc, sc, &ctx->comp_circ->c_sum);
            secp256k1_scalar_mul(sc, sc, &ctx->randomizer82);
            *pt = *ctx->genp;
            break;
        }
        /* A_I^x (83) */
        case 1:
            *sc = ctx->x;
            *pt = ctx->age[0];
            break;
        /* A_O^(x^2) (83) */
        case 2:
            secp256k1_scalar_sqr(sc, &ctx->x);
            *pt = ctx->age[1];
            break;
        /* S^(x^3) (83) */
        case 3:
            secp256k1_scalar_sqr(sc, &ctx->x); /* TODO cache previous squaring */
            secp256k1_scalar_mul(sc, sc, &ctx->x);
            *pt = ctx->age[2];
            break;
        /* T_1^x (82) */
        case 4:
            secp256k1_scalar_mul(sc, &ctx->x, &ctx->randomizer82);
            *pt = ctx->tge[0];
            break;
        default:
            if (ctx->count < 9) {
                size_t i;
                secp256k1_scalar_mul(sc, &ctx->x, &ctx->randomizer82);
                for (i = 0; i < ctx->count - 3; i++) {
                    secp256k1_scalar_mul(sc, sc, &ctx->x);
                }
                *pt = ctx->tge[ctx->count - 4];
            } else {
                /* V^(x^2 . (z^Q . W_V)) (82) */
                VERIFY_CHECK(!"bulletproof: too many points added by circuit_verify_impl to inner_product_verify_impl");
            }
        }
        secp256k1_scalar_mul(sc, sc, randomizer);
        ctx->count++;
    }
    return 1;
}

static int secp256k1_bulletproof_relation66_verify_impl(const secp256k1_ecmult_context *ecmult_ctx, secp256k1_scratch *scratch, const unsigned char* const* proof, size_t n_proofs, size_t plen, const secp256k1_ge *commitp, size_t nc, const secp256k1_ge *genp, const secp256k1_bulletproof_circuit* const* circ, const secp256k1_ge *gens, const size_t n_gens, const unsigned char *extra_commit, size_t extra_commit_len) {
    int ret;
    secp256k1_bulletproof_circuit_vfy_ecmult_context *ecmult_data;
    secp256k1_bulletproof_innerproduct_context *innp_ctx;
    size_t i;

    /* sanity-check input */
    if (plen < 64 + 256 + 1) {  /* inner product argument will do a more precise check */
        return 0;
    }
    if (plen > SECP256K1_BULLETPROOF_MAX_PROOF) {
        return 0;
    }

    if (!secp256k1_scratch_allocate_frame(scratch, n_proofs * (sizeof(*ecmult_data) + sizeof(*innp_ctx)), 2)) {
        return 0;
    }
    ecmult_data = (secp256k1_bulletproof_circuit_vfy_ecmult_context *)secp256k1_scratch_alloc(scratch, n_proofs * sizeof(*ecmult_data));
    innp_ctx = (secp256k1_bulletproof_innerproduct_context *)secp256k1_scratch_alloc(scratch, n_proofs * sizeof(*innp_ctx));
    if (!secp256k1_bulletproof_vfy_compressed_circuit_allocate_frame(scratch, circ[0], n_proofs)) {
        secp256k1_scratch_deallocate_frame(scratch);
        return 0;
    }

    for (i = 0; i < n_proofs; i++) {
        secp256k1_sha256 sha256;
        unsigned char randomizer82[32] = {0};  /* randomizer for eq (82) so we can add it to eq (83) to save a separate multiexp */
        unsigned char commit[32] = {0};
        secp256k1_scalar taux, mu;
        secp256k1_scalar y;
        int overflow;

        /* Commit to all input data: pedersen commit, asset generator, extra_commit */
        secp256k1_bulletproof_update_commit_n(commit, commitp, nc);
        secp256k1_bulletproof_update_commit_n(commit, genp, 1);
        if (extra_commit != NULL) {
            secp256k1_sha256_initialize(&sha256);
            secp256k1_sha256_write(&sha256, commit, 32);
            secp256k1_sha256_write(&sha256, extra_commit, extra_commit_len);
            secp256k1_sha256_finalize(&sha256, commit);
        }

        /* Deserialize everything */
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].age[0], &proof[i][64], 0, 8);
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].age[1], &proof[i][64], 1, 8);
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].age[2], &proof[i][64], 2, 8);
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].tge[0], &proof[i][64], 3, 8);
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].tge[1], &proof[i][64], 4, 8);
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].tge[2], &proof[i][64], 5, 8);
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].tge[3], &proof[i][64], 6, 8);
        secp256k1_bulletproof_deserialize_point(&ecmult_data[i].tge[4], &proof[i][64], 7, 8);

        /* Compute y, z, x */
        secp256k1_bulletproof_update_commit_n(commit, ecmult_data[i].age, 3);
        secp256k1_scalar_set_b32(&y, commit, &overflow);
        if (overflow || secp256k1_scalar_is_zero(&y)) {
            secp256k1_scratch_deallocate_frame(scratch);
            secp256k1_scratch_deallocate_frame(scratch);
            return 0;
        }
        ecmult_data[i].y = y;
        secp256k1_scalar_inverse_var(&ecmult_data[i].yinv, &y);  /* TODO batch this into another inverse */
        secp256k1_bulletproof_update_commit_n(commit, NULL, 0);
        secp256k1_scalar_set_b32(&ecmult_data[i].z, commit, &overflow);
        if (overflow || secp256k1_scalar_is_zero(&ecmult_data[i].z)) {
            secp256k1_scratch_deallocate_frame(scratch);
            secp256k1_scratch_deallocate_frame(scratch);
            return 0;
        }

        secp256k1_bulletproof_update_commit_n(commit, ecmult_data[i].tge, 5);
        secp256k1_scalar_set_b32(&ecmult_data[i].x, commit, &overflow);
        if (overflow || secp256k1_scalar_is_zero(&ecmult_data[i].x)) {
            secp256k1_scratch_deallocate_frame(scratch);
            secp256k1_scratch_deallocate_frame(scratch);
            return 0;
        }

        ecmult_data[i].comp_circ = secp256k1_bulletproof_vfy_compress_circuit(scratch, circ[i], &ecmult_data[i].x, &ecmult_data[i].y, &ecmult_data[i].yinv, &ecmult_data[i].z);

        /* Extract scalars */
        secp256k1_scalar_set_b32(&taux, &proof[i][0], &overflow);
        if (overflow || secp256k1_scalar_is_zero(&taux)) {
            secp256k1_scratch_deallocate_frame(scratch);
            secp256k1_scratch_deallocate_frame(scratch);
            return 0;
        }
        secp256k1_scalar_set_b32(&mu, &proof[i][32], &overflow);
        if (overflow || secp256k1_scalar_is_zero(&mu)) {
            secp256k1_scratch_deallocate_frame(scratch);
            secp256k1_scratch_deallocate_frame(scratch);
            return 0;
        }
        /* A little sketchy, we read t (l(x) . r(x)) off the front of the inner product proof,
         * which we otherwise treat as a black box */
        secp256k1_scalar_set_b32(&ecmult_data[i].t, &proof[i][64 + 256 + 1], &overflow);
        if (overflow || secp256k1_scalar_is_zero(&ecmult_data[i].t)) {
            secp256k1_scratch_deallocate_frame(scratch);
            secp256k1_scratch_deallocate_frame(scratch);
            return 0;
        }

        /* Mix these scalars into the hash so the input to the inner product proof is fixed */
        secp256k1_sha256_initialize(&sha256);
        secp256k1_sha256_write(&sha256, commit, 32);
        secp256k1_sha256_write(&sha256, proof[i], 64);
        secp256k1_sha256_finalize(&sha256, commit);

        secp256k1_sha256_initialize(&sha256);
        secp256k1_sha256_write(&sha256, commit, 32);
        secp256k1_sha256_finalize(&sha256, randomizer82);
        secp256k1_scalar_set_b32(&ecmult_data[i].randomizer82, randomizer82, &overflow);
        if (overflow || secp256k1_scalar_is_zero(&ecmult_data[i].randomizer82)) {
            secp256k1_scratch_deallocate_frame(scratch);
            secp256k1_scratch_deallocate_frame(scratch);
            return 0;
        }

        /* compute exponent offsets */
        ecmult_data[i].count = 0;

        ecmult_data[i].genp = genp;
        ecmult_data[i].commits = commitp;
        ecmult_data[i].n_gates = circ[i]->n_gates;
        ecmult_data[i].n_commits = nc;

        secp256k1_scalar_mul(&taux, &taux, &ecmult_data[i].randomizer82);
        secp256k1_scalar_add(&mu, &mu, &taux);

        innp_ctx[i].proof = &proof[i][64 + 256 + 1];
        innp_ctx[i].p_offs = mu;
        innp_ctx[i].yinv = ecmult_data[i].yinv;
        memcpy(innp_ctx[i].commit, commit, 32);
        innp_ctx[i].rangeproof_cb = secp256k1_bulletproof_circuit_vfy_callback;
        innp_ctx[i].rangeproof_cb_data = (void *) &ecmult_data[i];
        innp_ctx[i].n_extra_rangeproof_points = 9 + nc;
    }
    ret = secp256k1_bulletproof_inner_product_verify_impl(ecmult_ctx, scratch, gens, n_gens, circ[0]->n_gates, innp_ctx, n_proofs, plen - (64 + 256 + 1));
    secp256k1_scratch_deallocate_frame(scratch);
    secp256k1_scratch_deallocate_frame(scratch);
    return ret;
}

#endif
