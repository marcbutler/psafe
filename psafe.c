/* Copyright 2013-2015 Marc Butler <mockbutler@gmail.com>
 * All Rights Reserved
 */

#include <assert.h>
#include <err.h>
#include <gcrypt.h>
#include <getopt.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "crypto.h"
#include "ioport.h"
#include "pws3.h"
#include "util.h"

#include "psafe.h"

void stretch_key(const char* pass,
    size_t passlen,
    const struct psafe3_header* pro,
    uint8_t* skey)
{
    gcry_error_t gerr;
    gcry_md_hd_t sha256;
    gerr = gcry_md_open(&sha256, GCRY_MD_SHA256, GCRY_MD_FLAG_SECURE);
    if (gerr != GPG_ERR_NO_ERROR)
        gcrypt_fatal(gerr);

    gcry_md_write(sha256, pass, passlen);
    gcry_md_write(sha256, pro->salt, 32);
    memmove(skey, gcry_md_read(sha256, 0), 32);

    uint32_t iter = pro->iter;
    while (iter-- > 0) {
        gcry_md_reset(sha256);
        gcry_md_write(sha256, skey, 32);
        memmove(skey, gcry_md_read(sha256, 0), 32);
    }
    gcry_md_close(sha256);
}

void sha256_block32(const uint8_t* in, uint8_t* out)
{
    gcry_md_hd_t hd;
    gcry_error_t gerr;
    gerr = gcry_md_open(&hd, GCRY_MD_SHA256, GCRY_MD_FLAG_SECURE);
    if (gerr != GPG_ERR_NO_ERROR)
        gcrypt_fatal(gerr);
    gcry_md_write(hd, in, 32);
    gcry_md_final(hd);
    memmove(out, gcry_md_read(hd, 0), 32);
    gcry_md_close(hd);
}

void extract_random_key(const uint8_t* stretchkey,
    const uint8_t* fst,
    const uint8_t* snd,
    uint8_t* randkey)
{
    gcry_error_t gerr;
    gcry_cipher_hd_t hd;
    gerr = gcry_cipher_open(&hd, GCRY_CIPHER_TWOFISH, GCRY_CIPHER_MODE_ECB,
        GCRY_CIPHER_SECURE);
    if (gerr != GPG_ERR_NO_ERROR)
        gcrypt_fatal(gerr);
    gerr = gcry_cipher_setkey(hd, stretchkey, 32);
    if (gerr != GPG_ERR_NO_ERROR)
        gcrypt_fatal(gerr);
    gcry_cipher_decrypt(hd, randkey, 16, fst, 16);
    gcry_cipher_reset(hd);
    gcry_cipher_decrypt(hd, randkey + 16, 16, snd, 16);
    gcry_cipher_close(hd);
}

void print_time(uint8_t* val)
{
    struct tm* lt;
    time_t time;
    time = load_le32(val);
    lt = gmtime(&time);
    wprintf(L"%d-%d-%d %02d:%02d:%02d", 1900 + lt->tm_year, lt->tm_mon,
        lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
}

void printhex(FILE* f, uint8_t* ptr, unsigned cnt)
{
    unsigned i;
    for (i = 0; i < cnt; i++)
        fwprintf(f, L"%02x", *ptr++);
}

void print_uuid(uint8_t* uuid)
{
    printhex(stdout, uuid, 4);
    putwc('-', stdout);
    printhex(stdout, uuid + 4, 2);
    putwc('-', stdout);
    printhex(stdout, uuid + 6, 2);
    putwc('-', stdout);
    printhex(stdout, uuid + 8, 2);
    putwc('-', stdout);
    printhex(stdout, uuid + 10, 6);
}

/* Print out utf-8 string. */
void pws(FILE* f, uint8_t* bp, size_t len)
{
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    wchar_t* tmp;
    tmp = malloc((len + 1) * sizeof(wchar_t));
    size_t n;
    const char* ptr = (const char*)bp;
    n = mbsrtowcs(tmp, &ptr, len, &state);
    tmp[n] = L'\0';
    fputws(tmp, f);
    free(tmp);
}

void hd_print(FILE* f, struct field* fld)
{
    switch (fld->type) {
    case 0x2 ... 0x3:
    case 0x5 ... 0xb:
    case 0xf ... 0x11:
        pws(f, fld->val, fld->len);
        break;
    case 0x1:
        print_uuid(fld->val);
        break;
    case 0x4:
        print_time(fld->val);
        break;
    }
}

void db_print(FILE* f, struct field* fld)
{
    switch (fld->type) {
    case 0x2 ... 0x6:
    case 0xd ... 0x10:
    case 0x14:
    case 0x16:
        pws(f, fld->val, fld->len);
        break;
    case 0x7 ... 0xa:
    case 0xc:
        print_time(fld->val);
        break;
    case 0x1:
        print_uuid(fld->val);
        break;
    }
}

int init_decrypt_ctx(struct crypto_ctx* ctx,
    struct psafe3_header* pro,
    struct safe_sec* sec)
{
    gcry_error_t gerr;

    assert(ctx != NULL);
    assert(pro != NULL);
    assert(sec != NULL);

    gerr = gcry_cipher_open(&ctx->cipher, GCRY_CIPHER_TWOFISH,
        GCRY_CIPHER_MODE_CBC, GCRY_CIPHER_SECURE);
    if (gerr != GPG_ERR_NO_ERROR)
        goto err_cipher;

    ctx->gerr = gcry_cipher_setkey(ctx->cipher, sec->rand_k, 32);
    if (gerr != GPG_ERR_NO_ERROR)
        goto err_cipher;

    ctx->gerr = gcry_cipher_setiv(ctx->cipher, pro->iv, 16);
    if (gerr != GPG_ERR_NO_ERROR)
        goto err_cipher;

    gerr = gcry_md_open(&ctx->hmac, GCRY_MD_SHA256,
        GCRY_MD_FLAG_SECURE | GCRY_MD_FLAG_HMAC);
    if (gerr != GPG_ERR_NO_ERROR)
        goto err_hmac;

    gerr = gcry_md_setkey(ctx->hmac, sec->rand_l, 32);
    if (gerr != GPG_ERR_NO_ERROR)
        goto err_hmac;

    return 0;

err_hmac:
    gcry_cipher_close(ctx->cipher);
err_cipher:
    ctx->gerr = gerr;
    return -1;
}

void term_decrypt_ctx(struct crypto_ctx* ctx)
{
    gcry_cipher_close(ctx->cipher);
    gcry_md_close(ctx->hmac);
}

void print_prologue(FILE* f, struct psafe3_header* pro)
{
    int i;
#define EOL() fputwc('\n', f)
    fputws(L"SALT   ", f);
    printhex(f, pro->salt, 32);
    EOL();
    fwprintf(f, L"ITER   %u\n", pro->iter);
    fputws(L"H(P')  ", f);
    printhex(f, pro->h_pprime, 32);
    EOL();
    for (i = 0; i < 4; i++) {
        fwprintf(f, L"B%d     ", i);
        printhex(f, pro->b[i], 16);
        EOL();
    }
    fputws(L"IV     ", f);
    printhex(f, pro->iv, 16);
    EOL();
#undef EOL
}

int stretch_and_check_pass(const char* pass,
    size_t passlen,
    struct psafe3_header* pro,
    struct safe_sec* sec)
{
    stretch_key(pass, passlen, pro, sec->pprime);
    uint8_t hkey[32];
    sha256_block32(sec->pprime, hkey);
    if (memcmp(pro->h_pprime, hkey, 32) != 0)
        return -1;
    extract_random_key(sec->pprime, pro->b[0], pro->b[1], sec->rand_k);
    extract_random_key(sec->pprime, pro->b[2], pro->b[3], sec->rand_l);
    return 0;
}

int main(int argc, char** argv)
{
    int ret;
    setlocale(LC_ALL, "");

    if (argc != 3) {
        wprintf(L"Usage: psafe file.psafe3 passphrase\n");
        exit(EXIT_FAILURE);
    }

    crypto_init(64 * 1024);

    struct ioport* safe_io = NULL;
    if (ioport_mmap_open(argv[1], &safe_io) != 0) {
        err(1, "%s", argv[1]);
    }

    struct ioport_mmap* mmio = (void*)safe_io;
    uint8_t* ptr = mmio->mem;
    size_t sz = mmio->mem_size;
    struct psafe3_header hdr;
    if (pws3_read_header(safe_io, &hdr) != 0) {
        fwprintf(stderr, L"Error reading header.");
        exit(EXIT_FAILURE);
    }

    struct safe_sec* sec;
    sec = gcry_malloc_secure(sizeof(*sec));
    ret = stretch_and_check_pass(argv[2], strlen(argv[2]), &hdr, sec);
    if (ret != 0) {
        gcry_free(sec);
        wprintf(L"Invalid password.\n");
        exit(1);
    }

    uint8_t* safe;
    size_t safe_size;
    safe_size = sz - (4 + sizeof(hdr) + 48);
    assert(safe_size > 0);
    assert(safe_size % TWOFISH_SIZE == 0);
    safe = gcry_malloc_secure(safe_size);
    assert(safe != NULL);

    gcry_error_t gerr;
    struct crypto_ctx ctx;
    if (init_decrypt_ctx(&ctx, &hdr, sec) < 0)
        gcrypt_fatal(ctx.gerr);

    size_t bcnt;
    bcnt = safe_size / TWOFISH_SIZE;
    assert(bcnt > 0);
    uint8_t* encp;
    uint8_t* safep;
    encp = ptr + 4 + sizeof(hdr);
    safep = safe;
    while (bcnt-- && !safe_io->drained(safe_io)) {
        gerr = gcry_cipher_decrypt(ctx.cipher, safep, TWOFISH_SIZE, encp,
            TWOFISH_SIZE);
        if (gerr != GPG_ERR_NO_ERROR)
            gcrypt_fatal(gerr);
        safep += TWOFISH_SIZE;
        encp += TWOFISH_SIZE;
    }

    enum { HDR,
        DB };
    int state = HDR;
    safep = safe;
    while (safep < safe + safe_size) {
        struct field* fld;
        fld = (struct field*)safep;
        wprintf(L"len=%-3u  type=%02x  ", fld->len, fld->type);
        if (state == DB)
            db_print(stdout, fld);
        else
            hd_print(stdout, fld);
        if (fld->type == 0xff)
            state = DB;
        putwc('\n', stdout);
        if (fld->len)
            gcry_md_write(ctx.hmac, safep + sizeof(*fld), fld->len);
        safep += ((fld->len + 5 + 15) / TWOFISH_SIZE) * TWOFISH_SIZE;
    }

    assert(memcmp(ptr + (sz - 48), "PWS3-EOFPWS3-EOF", TWOFISH_SIZE) == 0);

#define EOL() putwc('\n', stdout)
    EOL();
    print_prologue(stdout, &hdr);
    wprintf(L"KEY    ");
    printhex(stdout, sec->pprime, 32);
    EOL();
    wprintf(L"H(KEY) ");
    printhex(stdout, hdr.h_pprime, 32);
    EOL();

    gcry_md_final(ctx.hmac);
    wprintf(L"HMAC'  ");
    uint8_t hmac[32];
    memmove(hmac, gcry_md_read(ctx.hmac, GCRY_MD_SHA256), 32);
    printhex(stdout, hmac, 32);
    EOL();

    wprintf(L"HMAC   ");
    printhex(stdout, ptr + (sz - 32), 32);
    EOL();
#undef EOL

    gcry_free(safe);
    gcry_free(sec);

    safe_io->close(safe_io);
    term_decrypt_ctx(&ctx);

    crypto_term();
    exit(0);
}
