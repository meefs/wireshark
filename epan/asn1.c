/* asn1.c
 * Common routines for ASN.1
 * 2007  Anders Broman
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib.h>

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <epan/packet.h>

#include "asn1.h"

void asn1_ctx_init(asn1_ctx_t *actx, asn1_enc_e encoding, bool aligned, packet_info *pinfo) {
  memset(actx, '\0', sizeof(*actx));
  actx->signature = ASN1_CTX_SIGNATURE;
  actx->encoding = encoding;
  actx->aligned = aligned;
  actx->pinfo = pinfo;
}

bool asn1_ctx_check_signature(asn1_ctx_t *actx) {
  return actx && (actx->signature == ASN1_CTX_SIGNATURE);
}

void asn1_ctx_clean_external(asn1_ctx_t *actx) {
  memset(&actx->external, '\0', sizeof(actx->external));
  actx->external.hf_index = -1;
  actx->external.encoding = -1;
}

void asn1_ctx_clean_epdv(asn1_ctx_t *actx) {
  memset(&actx->embedded_pdv, '\0', sizeof(actx->embedded_pdv));
  actx->embedded_pdv.hf_index = -1;
  actx->embedded_pdv.identification = -1;
}


/*--- stack/parameters ---*/

void asn1_stack_frame_push(asn1_ctx_t *actx, const char *name) {
  asn1_stack_frame_t *frame;

  frame = wmem_new0(actx->pinfo->pool, asn1_stack_frame_t);
  frame->name = name;
  frame->next = actx->stack;
  actx->stack = frame;
}

void asn1_stack_frame_pop(asn1_ctx_t *actx, const char *name) {
  DISSECTOR_ASSERT(actx->stack);
  DISSECTOR_ASSERT(!strcmp(actx->stack->name, name));
  actx->stack = actx->stack->next;
}

void asn1_stack_frame_check(asn1_ctx_t *actx, const char *name, const asn1_par_def_t *par_def) {
  const asn1_par_def_t *pd = par_def;
  asn1_par_t *par;

  DISSECTOR_ASSERT(actx->stack);
  DISSECTOR_ASSERT(!strcmp(actx->stack->name, name));

  par = actx->stack->par;
  while (pd->name) {
    DISSECTOR_ASSERT(par);
    DISSECTOR_ASSERT((pd->ptype == ASN1_PAR_IRR) || (par->ptype == pd->ptype));
    par->name = pd->name;
    pd++;
    par = par->next;
  }
  DISSECTOR_ASSERT(!par);
}

static asn1_par_t *get_par_by_name(asn1_ctx_t *actx, const char *name) {
  asn1_par_t *par = NULL;

  DISSECTOR_ASSERT(actx->stack);
  par = actx->stack->par;
  while (par) {
    if (!strcmp(par->name, name))
      return par;
    par = par->next;
  }
  return par;
}

static asn1_par_t *push_new_par(asn1_ctx_t *actx) {
  asn1_par_t *par, **pp;

  DISSECTOR_ASSERT(actx->stack);

  par = wmem_new0(actx->pinfo->pool, asn1_par_t);

  pp = &(actx->stack->par);
  while (*pp)
    pp = &((*pp)->next);
  *pp = par;

  return par;
}

void asn1_param_push_boolean(asn1_ctx_t *actx, bool value) {
  asn1_par_t *par;

  par = push_new_par(actx);
  par->ptype = ASN1_PAR_BOOLEAN;
  par->value.v_boolean = value;
}

void asn1_param_push_integer(asn1_ctx_t *actx, int32_t value) {
  asn1_par_t *par;

  par = push_new_par(actx);
  par->ptype = ASN1_PAR_INTEGER;
  par->value.v_integer = value;
}

bool asn1_param_get_boolean(asn1_ctx_t *actx, const char *name) {
  asn1_par_t *par = NULL;

  par = get_par_by_name(actx, name);
  DISSECTOR_ASSERT(par);
  return par->value.v_boolean;
}

int32_t asn1_param_get_integer(asn1_ctx_t *actx, const char *name) {
  asn1_par_t *par = NULL;

  par = get_par_by_name(actx, name);
  DISSECTOR_ASSERT(par);
  return par->value.v_integer;
}


/*--- ROSE ---*/

void rose_ctx_init(rose_ctx_t *rctx) {
  memset(rctx, '\0', sizeof(*rctx));
  rctx->signature = ROSE_CTX_SIGNATURE;
}

bool rose_ctx_check_signature(rose_ctx_t *rctx) {
  return rctx && (rctx->signature == ROSE_CTX_SIGNATURE);
}

void rose_ctx_clean_data(rose_ctx_t *rctx) {
  memset(&rctx->d, '\0', sizeof(rctx->d));
  rctx->d.code = -1;
}

asn1_ctx_t *get_asn1_ctx(void *ptr) {
  asn1_ctx_t *actx = (asn1_ctx_t*)ptr;

  if (!asn1_ctx_check_signature(actx))
    actx = NULL;

  return actx;
}

rose_ctx_t *get_rose_ctx(void *ptr) {
  rose_ctx_t *rctx = (rose_ctx_t*)ptr;
  asn1_ctx_t *actx = (asn1_ctx_t*)ptr;

  if (!asn1_ctx_check_signature(actx))
    actx = NULL;

  if (actx)
    rctx = actx->rose_ctx;

  if (!rose_ctx_check_signature(rctx))
    rctx = NULL;

  return rctx;
}

double asn1_get_real(const uint8_t *real_ptr, int len) {
  uint8_t octet;
  const uint8_t *p;
  uint8_t *buf;
  double val = 0;

  /* 8.5.2    If the real value is the value zero,
   *          there shall be no contents octets in the encoding.
   */
  if (len < 1) return val;

  octet = real_ptr[0];
  p = real_ptr + 1;
  len -= 1;
  if (octet & 0x80) {  /* binary encoding */
    int i;
    bool Eneg;
    int8_t S; /* Sign */
    uint8_t B; /* Base */
    uint8_t F; /* scaling Factor */
    int32_t E = 0; /* Exponent (supported max 3 octets/24 bit) */
    uint64_t N = 0; /* N (supported max 8 octets/64 bit) */

    uint8_t lenE, lenN;

    if(octet & 0x40) S = -1; else S = 1;
    switch(octet & 0x30) {
      case 0x00: B = 2; break;
      case 0x10: B = 8; break;
      case 0x20: B = 16; break;
      case 0x30: /* Reserved */
      default:
        /* TODO Add some warning in tree about reserved value for Base */
        return 0;
    }
    F = (octet & 0x0c) >> 2;

    /* 8.5.6.4 Exponent length */
    lenE = (octet & 0x3) + 1;

    /* we can't handle exponents > 24 bits */
    /* TODO Next octet(s) define length of exponent */
    DISSECTOR_ASSERT(lenE != 4);

    /* Ensure the buffer len and its content are coherent */
    DISSECTOR_ASSERT(lenE < len);

    Eneg = ((*p) & 0x80) ? true : false;
    for (i = 0; i < lenE; i++) {
      if(Eneg) {
        /* 2's complement: inverse bits */
        E = (E<<8) | ((uint8_t) ~(*p));
      } else {
        E = (E<<8) | *p;
      }
      p++;
    }
    if(Eneg) {
      /* 2's complement: ... and add 1 (and make negative of course) */
      E = -(E + 1);
    }

    lenN = len - lenE;

    /* we can't handle integers > 64 bits */
    DISSECTOR_ASSERT(lenN <= 8);

    for (i=0; i<lenN; i++) {
      N = (N<<8) | *p;
      p++;
    }
    val = (double) S * N * pow(2, F) * pow(B, E);
  } else if (octet & 0x40) {  /* SpecialRealValue */
    switch (octet & 0x3F) {
      case 0x00: val = HUGE_VAL; break;
      case 0x01: val = -HUGE_VAL; break;
      case 0x02: val = NAN; break;
    }
  } else {  /* decimal encoding */
    buf = g_strndup(p, len);
    val = g_ascii_strtod(buf, NULL);
    g_free(buf);
  }

  return val;
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local Variables:
 * c-basic-offset: 2
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=2 tabstop=8 expandtab:
 * :indentSize=2:tabSize=8:noTabs=true:
 */
