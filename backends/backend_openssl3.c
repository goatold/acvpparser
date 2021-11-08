/*
 * Copyright 2021 VMware, Inc.
 *
 * License: see LICENSE file
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * The code uses the interface offered by OpenSSL-3
 */

#include "backend_openssl_common.h"

OSSL_PROVIDER *fips;
OSSL_PROVIDER *base;

ACVP_DEFINE_CONSTRUCTOR(openssl_backend_init)
static void openssl_backend_init(void)
{
	/* Explicitly load the FIPS provider as per fips_module(7) */
	fips = OSSL_PROVIDER_load(NULL, "fips");
	if (fips == NULL) {
		printf("Failed to load FIPS provider\n");
		exit(-EFAULT);
	}
	base = OSSL_PROVIDER_load(NULL, "base");
	if (base == NULL) {
		OSSL_PROVIDER_unload(fips);
		printf("Failed to load base provider\n");
		exit(-EFAULT);
	}
}

ACVP_DEFINE_DESTRUCTOR(openssl_backend_fini)
static void openssl_backend_fini(void)
{
	OSSL_PROVIDER_unload(base);
	OSSL_PROVIDER_unload(fips);
}

/************************************************
 * General helper functions
 ************************************************/
static void openssl_dh_get0_key(const EVP_PKEY *r, BIGNUM **pub_key,
				BIGNUM **priv_key)
{
	EVP_PKEY_get_bn_param(r,OSSL_PKEY_PARAM_PRIV_KEY, priv_key);
	EVP_PKEY_get_bn_param(r,OSSL_PKEY_PARAM_PUB_KEY, pub_key);
}

/************************************************
 * CMAC/HMAC cipher interface functions
 ************************************************/
static int openssl_mac_generate_helper(struct hmac_data *data, char *mac_algo,
				       char *param_name, char *param_val)
{
	EVP_MAC_CTX *ctx = NULL;
	EVP_MAC *mac = NULL;
	OSSL_PARAM params[3], *p;
	int ret = 0;

	mac = EVP_MAC_fetch(NULL, mac_algo, NULL);
	CKNULL(mac, -EFAULT);
	ctx = EVP_MAC_CTX_new(mac);
	CKNULL(ctx, -EFAULT);

	p = params;
	// OpenSSL wants us to use the cipher name here...
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY, data->key.buf, data->key.len);
	*p++ = OSSL_PARAM_construct_utf8_string(param_name, param_val, 0);
	*p = OSSL_PARAM_construct_end();

	CKINT_O_LOG(EVP_MAC_CTX_set_params(ctx, params),
			"EVP_MAC_CTX_set_params failed\n");
	CKINT_O_LOG(EVP_MAC_init(ctx, NULL, 0, NULL),
			"EVP_MAC_init failed\n");

	CKINT_O_LOG(EVP_MAC_update(ctx, data->msg.buf, data->msg.len),
			"EVP_MAC_update failed\n");
	CKINT_LOG(alloc_buf((size_t)EVP_MAC_CTX_get_mac_size(ctx), &data->mac),
			"%s buffer cannot be allocated\n", mac_algo);
	CKINT_O_LOG(EVP_MAC_final(ctx, data->mac.buf, &data->mac.len, data->mac.len),
			"EVP_MAC_final failed\n");

	logger(LOGGER_DEBUG, "taglen = %zu\n", data->mac.len);
	logger_binary(LOGGER_DEBUG, data->mac.buf, data->mac.len, mac_algo);

	ret = 0;

out:
	if (mac)
		EVP_MAC_free(mac);
	if (ctx)
		EVP_MAC_CTX_free(ctx);
	return ret;
}

static int openssl_cmac_generate(struct hmac_data *data)
{
	const EVP_CIPHER *type = NULL;
	int ret = 0;

	logger_binary(LOGGER_DEBUG, data->msg.buf, data->msg.len, "msg");
	logger_binary(LOGGER_DEBUG, data->key.buf, data->key.len, "key");

	CKINT(openssl_cipher(data->cipher, data->key.len, &type));

	if (openssl_mac_generate_helper(data, "CMAC", OSSL_MAC_PARAM_CIPHER,
		(char *)EVP_CIPHER_name(type)))
	{
		ret = -EFAULT;
		goto out;
	}

	// Truncate to desired macLen, which is in bits
	if (data->mac.len > data->maclen / 8) {
		data->mac.buf[data->maclen / 8] = '\0';
		data->mac.len = data->maclen / 8;
		logger(LOGGER_DEBUG, "Truncated mac to maclen: %d\n", data->maclen);
		logger_binary(LOGGER_DEBUG, data->mac.buf, data->mac.len, "mac");
	}

	ret = 0;

out:
	return ret;
}

static int openssl_hmac_generate(struct hmac_data *data)
{
	const EVP_MD *md = NULL;
	int ret = 0;

	logger_binary(LOGGER_DEBUG, data->msg.buf, data->msg.len, "msg");
	logger_binary(LOGGER_DEBUG, data->key.buf, data->key.len, "key");

	CKINT(openssl_md_convert(data->cipher, &md));

	if (openssl_mac_generate_helper(data, "HMAC", OSSL_MAC_PARAM_DIGEST,
		(char *)EVP_MD_name(md)))
	{
		ret = -EFAULT;
		goto out;
	}

	ret = 0;

out:
	return ret;
}

static int openssl_mac_generate(struct hmac_data *data, flags_t parsed_flags)
{
	(void)parsed_flags;

	switch(data->cipher) {
	case ACVP_AESCMAC:
	case ACVP_TDESCMAC:
		return openssl_cmac_generate(data);
		break;
	default:
		return openssl_hmac_generate(data);
		break;
	}

	return -EFAULT;
}

static struct hmac_backend openssl_mac =
{
	openssl_mac_generate,
};

ACVP_DEFINE_CONSTRUCTOR(openssl_mac_backend)
static void openssl_mac_backend(void)
{
	register_hmac_impl(&openssl_mac);
}

/************************************************
 * KMAC cipher interface functions
 ************************************************/
static int openssl_kmac_generate(struct kmac_data *data, flags_t parsed_flags)
{
	EVP_MAC_CTX *ctx = NULL;
	EVP_MAC *mac = NULL;
	OSSL_PARAM params[4], *p;
	int blocklen = (int) data->maclen/8;
	int ret =0;
	int xof_enabled =0;
	const char *algo;
	(void)parsed_flags;

	logger_binary(LOGGER_DEBUG, data->msg.buf, data->msg.len, "msg");
	logger_binary(LOGGER_DEBUG, data->key.buf, data->key.len, "key");

	convert_cipher_algo(data->cipher & ACVP_KMACMASK, ACVP_CIPHERTYPE_KMAC, &algo);

	mac = EVP_MAC_fetch(NULL, algo, NULL);
	CKNULL(mac, -EFAULT);
	ctx = EVP_MAC_CTX_new(mac);
	CKNULL(ctx, -EFAULT);

	p=params;
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY, data->key.buf, data->key.len);
	if (data->customization.buf != NULL && data->customization.len != 0)
		*p++ = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_CUSTOM, data->customization.buf, data->customization.len);
	*p = OSSL_PARAM_construct_end();

	CKINT_O_LOG(EVP_MAC_CTX_set_params(ctx, params),
			"EVP_MAC_CTX_set_params failed\n");
	CKINT_O_LOG(EVP_MAC_init(ctx, NULL, 0, NULL),
			"EVP_MAC_init failed\n");

	xof_enabled =(int)data->xof_enabled;

	p = params;
	*p++ = OSSL_PARAM_construct_int(OSSL_MAC_PARAM_XOF, &xof_enabled);
	*p++ = OSSL_PARAM_construct_int(OSSL_MAC_PARAM_SIZE, &blocklen);
	*p = OSSL_PARAM_construct_end();

	CKINT_O_LOG(EVP_MAC_CTX_set_params(ctx, params),
			"EVP_MAC_CTX_set_params failed\n");

	CKINT_O_LOG(EVP_MAC_update(ctx, data->msg.buf, data->msg.len),
			"EVP_MAC_update failed\n");
	CKINT_LOG(alloc_buf((size_t)blocklen, &data->mac),
			"KMAC buffer cannot be allocated\n");
	CKINT_O_LOG(EVP_MAC_final(ctx, data->mac.buf, &data->mac.len, blocklen),
			"EVP_MAC_final failed\n");

	logger(LOGGER_DEBUG, "taglen = %zu\n", data->mac.len);
	logger_binary(LOGGER_DEBUG, data->mac.buf, data->mac.len, "KMAC");
out:
	if(mac)
		EVP_MAC_free(mac);
	if(ctx)
		EVP_MAC_CTX_free(ctx);
	return 0;
}

static int openssl_kmac_ver(struct kmac_data *data, flags_t parsed_flags)
{
	EVP_MAC_CTX *ctx = NULL;
	EVP_MAC *mac = NULL;
	OSSL_PARAM params[4], *p;
	BUFFER_INIT(kmac);
	int blocklen = (int) data->maclen/8;
	size_t maclen =0;
	int ret =0;
	int xof_enabled =0;
	const char *algo;
	(void)parsed_flags;

	logger_binary(LOGGER_DEBUG, data->msg.buf, data->msg.len, "msg");
	logger_binary(LOGGER_DEBUG, data->key.buf, data->key.len, "key");

	convert_cipher_algo(data->cipher & ACVP_KMACMASK, ACVP_CIPHERTYPE_KMAC, &algo);

	mac = EVP_MAC_fetch(NULL, algo, NULL);
	CKNULL(mac, -EFAULT);
	ctx = EVP_MAC_CTX_new(mac);
	CKNULL(ctx, -EFAULT);
	if(mac)
		EVP_MAC_free(mac);

	p=params;
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY, data->key.buf, data->key.len);
	if (data->customization.buf != NULL && data->customization.len != 0)
		*p++ = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_CUSTOM, data->customization.buf, data->customization.len);
	*p = OSSL_PARAM_construct_end();

	CKINT_O_LOG(EVP_MAC_CTX_set_params(ctx, params),
			"EVP_MAC_CTX_set_params failed\n");
	CKINT_O_LOG(EVP_MAC_init(ctx,NULL,0,NULL),
			"EVP_MAC_init failed\n");

	xof_enabled =(int)data->xof_enabled;

	p = params;
	*p++ = OSSL_PARAM_construct_int(OSSL_MAC_PARAM_XOF, &xof_enabled);
	*p++ = OSSL_PARAM_construct_int(OSSL_MAC_PARAM_SIZE, &blocklen);
	*p = OSSL_PARAM_construct_end();

	CKINT_O_LOG(EVP_MAC_CTX_set_params(ctx, params),
			"EVP_MAC_CTX_set_params failed\n");
	CKINT_O_LOG(EVP_MAC_update(ctx, data->msg.buf, data->msg.len),
			"EVP_MAC_update failed\n");
	CKINT_LOG(alloc_buf((size_t)blocklen, &kmac),
			"KMAC buffer cannot be allocated\n");
	CKINT_O_LOG(EVP_MAC_final(ctx, kmac.buf, &maclen, blocklen),
			"EVP_MAC_update failed\n");

	logger(LOGGER_DEBUG, "taglen = %zu\n", maclen);
	logger_binary(LOGGER_DEBUG, kmac.buf, maclen, "Generated KMAC");
	logger_binary(LOGGER_DEBUG, data->mac.buf, data->mac.len, "Input KMAC");

	if(memcmp(data->mac.buf,kmac.buf,data->mac.len))
		data->verify_result = 0;
	else
		data->verify_result = 1;

	logger(LOGGER_DEBUG, "Generated result= %" PRIu32 "\n",data->verify_result);

out:
	if(ctx)
		EVP_MAC_CTX_free(ctx);
	free_buf(&kmac);
	return 0;
}

static struct kmac_backend openssl_kmac =
{
	openssl_kmac_generate,
	openssl_kmac_ver
};

ACVP_DEFINE_CONSTRUCTOR(openssl_kmac_backend)
static void openssl_kmac_backend(void)
{
	register_kmac_impl(&openssl_kmac);
}

/************************************************
 * DH interface functions
 ************************************************/
static int openssl_dh_ss_common(uint64_t cipher,
				uint64_t safeprime,
				struct buffer *P,
				struct buffer *Q,
				struct buffer *G,
				struct buffer *Yrem,
				struct buffer *Xloc,
				struct buffer *Yloc,
				struct buffer *hashzz)
{
	EVP_PKEY_CTX *pctx = NULL;
	EVP_PKEY_CTX *dctx = NULL;		/* Create a EVP_PKEY_CTX to perform key derivation */
	EVP_PKEY *pkey = NULL;
	EVP_PKEY *peerkey = NULL;
	OSSL_PARAM_BLD *bld = NULL;
	OSSL_PARAM *params = NULL;
	OSSL_PARAM *params_peer = NULL;
	EVP_PKEY *genkey = NULL;
    EVP_PKEY_CTX *gctx = NULL;
	BIGNUM *p_bn = NULL, *q_bn = NULL,*g_bn = NULL;
	BIGNUM *bn_Yrem = NULL, *bn_Xloc = NULL, *bn_Yloc = NULL;
	BIGNUM *cbn_Xloc = NULL, *cbn_Yloc = NULL;
	BUFFER_INIT(ss);
	unsigned int localkey_consumed = 0;
	size_t keylen = 0;
	int ret = 0;
	(void) safeprime;

	bld = OSSL_PARAM_BLD_new();
	p_bn = BN_bin2bn((const unsigned char *)P->buf, (int)P->len, NULL);
	OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_P, p_bn);
	q_bn = BN_bin2bn((const unsigned char *)Q->buf, (int)Q->len, NULL);
	OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_Q, q_bn);
	g_bn = BN_bin2bn((const unsigned char *)G->buf, (int)G->len, NULL);
	OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_G, g_bn);

	if (!Xloc->len || !Yloc->len) {
		pctx = EVP_PKEY_CTX_new_from_name(NULL, "DH", NULL);
		CKNULL(pctx, -EFAULT);
		CKINT_O_LOG(EVP_PKEY_fromdata_init(pctx),
					"EVP_PKEY_fromdata_init failed\n");

		params = OSSL_PARAM_BLD_to_param(bld);
		CKINT_O_LOG(EVP_PKEY_fromdata(pctx, &genkey, EVP_PKEY_KEYPAIR, params),
					"EVP_PKEY_keygen failed\n");
		gctx = EVP_PKEY_CTX_new_from_pkey(NULL, genkey, NULL);
		CKNULL(gctx, -EFAULT);
		CKINT_O_LOG(EVP_PKEY_keygen_init(gctx),
					"EVP_PKEY_keygen_init failed\n");
		CKINT_O_LOG(EVP_PKEY_generate(gctx, &pkey),
					"EVP_PKEY_generate failed\n");

		openssl_dh_get0_key(pkey, &cbn_Yloc, &cbn_Xloc);
		CKINT(openssl_bn2buffer(cbn_Yloc, Yloc));
		logger_binary(LOGGER_DEBUG, Yloc->buf, Yloc->len,
			      "generated Yloc");
	} else {
		logger_binary(LOGGER_DEBUG, Xloc->buf, Xloc->len, "used Xloc");
		bn_Xloc = BN_bin2bn((const unsigned char *)Xloc->buf,
				    (int)Xloc->len, NULL);
		CKNULL_LOG(bn_Xloc, -ENOMEM, "BN_bin2bn() failed\n");
		OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, bn_Xloc);
		localkey_consumed = 1;
		params = OSSL_PARAM_BLD_to_param(bld);

		pctx = EVP_PKEY_CTX_new_from_name(NULL, "DH", NULL);
		CKNULL(pctx, -EFAULT);
		CKINT_O_LOG(EVP_PKEY_fromdata_init(pctx),
					"EVP_PKEY_fromdata_init failed\n");
		EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_KEYPAIR, params);
	}

	OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_P, p_bn);
	OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_Q, q_bn);
	OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_FFC_G, g_bn);
	logger_binary(LOGGER_DEBUG, Yrem->buf, Yrem->len, "Yremote");
	bn_Yrem = BN_bin2bn((const unsigned char *)Yrem->buf, (int)Yrem->len, NULL);
	OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PUB_KEY, bn_Yrem);
	params_peer = OSSL_PARAM_BLD_to_param(bld);

	pctx = EVP_PKEY_CTX_new_from_name(NULL, "DH", NULL);
	CKNULL(pctx, -EFAULT);
	CKINT_O_LOG(EVP_PKEY_fromdata_init(pctx),
			"EVP_PKEY_fromdata_init failed\n");
	CKINT_O_LOG(EVP_PKEY_fromdata(pctx, &peerkey, EVP_PKEY_PUBLIC_KEY, params_peer),
			"EVP_PKEY_fromdata failed\n");
	CKINT_LOG(alloc_buf(keylen, &ss), "Cannot allocate ss\n");

	/* Compute the shared secret */
	dctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
	CKNULL(dctx, -EFAULT);
	CKINT_O_LOG(EVP_PKEY_derive_init(dctx),
			"EVP_PKEY_derive_init failed\n");
	if(EVP_PKEY_derive_set_peer(dctx, peerkey)<0){
		ERR_print_errors_fp(stderr);
		goto out;
	}
	CKINT_O_LOG(EVP_PKEY_derive(dctx, NULL, &ss.len),
			"EVP_PKEY_derive failed\n");
	CKINT(alloc_buf(ss.len, &ss));
	if(EVP_PKEY_derive(dctx, ss.buf, &ss.len)<=0){
		ERR_print_errors_fp(stderr);
		goto out;
	}
	ret = openssl_hash_ss(cipher, &ss, hashzz);
	logger_binary(LOGGER_DEBUG, ss.buf, ss.len, "Generated shared secret");

	/* We do not use CKINT here, because -ENOENT is no real error */
out:
	if(pkey)
		EVP_PKEY_free(pkey);
	if(peerkey)
		EVP_PKEY_free(peerkey);
	if(pctx)
		EVP_PKEY_CTX_free(pctx);
	if(dctx)
		EVP_PKEY_CTX_free(dctx);
	if(params_peer)
		OSSL_PARAM_free(params_peer);
	if(params)
		OSSL_PARAM_free(params);
	if(bld)
		OSSL_PARAM_BLD_free(bld);
	if (bn_Yrem)
		BN_free(bn_Yrem);
	if (!localkey_consumed && bn_Xloc)
		BN_free(bn_Xloc);
	if (!localkey_consumed && bn_Yloc)
		BN_free(bn_Yloc);
	free_buf(&ss);
	return ret;
}

static int openssl_dh_ss(struct dh_ss_data *data, flags_t parsed_flags)
{
	(void)parsed_flags;

	return openssl_dh_ss_common(data->cipher, data->safeprime,
					&data->P, &data->Q, &data->G,
					&data->Yrem,
					&data->Xloc, &data->Yloc,
					&data->hashzz);
}

static int openssl_dh_ss_ver(struct dh_ss_ver_data *data,
			       flags_t parsed_flags)
{
	int ret = openssl_dh_ss_common(data->cipher, data->safeprime,
					&data->P, &data->Q,
					&data->G,
					&data->Yrem,
					&data->Xloc, &data->Yloc,
					&data->hashzz);

	(void)parsed_flags;

	if (ret == -EOPNOTSUPP || ret == -ENOENT) {
		data->validity_success = 0;
		logger(LOGGER_DEBUG, "DH validity test failed\n");
		return 0;
	} else if (!ret) {
		data->validity_success = 1;
		logger(LOGGER_DEBUG, "DH validity test passed\n");
		return 0;
	}

	logger(LOGGER_DEBUG, "DH validity test: general error\n");
	return ret;
}

static struct dh_backend openssl_dh =
{
	openssl_dh_ss,
	openssl_dh_ss_ver,
};

ACVP_DEFINE_CONSTRUCTOR(openssl_dh_backend)
static void openssl_dh_backend(void)
{
	register_dh_impl(&openssl_dh);
}

/************************************************
 * ECDH cipher interface functions
 ************************************************/

static int openssl_ecdh_ss_common(uint64_t cipher,
		struct buffer *Qxrem, struct buffer *Qyrem,
		struct buffer *privloc,
		struct buffer *Qxloc, struct buffer *Qyloc,
		struct buffer *hashzz)
{
	int nid = 0, ret = 0;
	EVP_PKEY_CTX *kactx = NULL, *dctx = NULL;
	EVP_PKEY *pkey = NULL, *remotekey = NULL;
	OSSL_PARAM *params = NULL;
	OSSL_PARAM *params_remote = NULL;
	OSSL_PARAM_BLD *bld = NULL;
	BUFFER_INIT(ss);
	BUFFER_INIT(publoc);
	BUFFER_INIT(pubrem);
	BIGNUM  *privloc_bn = NULL;
	char dgst[50];

	bld = OSSL_PARAM_BLD_new();

	CKINT_LOG(_openssl_ecdsa_curves(cipher, &nid , dgst),
			"Conversion of curve failed\n");
	const char *digest = dgst;
	OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, digest, 0);
	OSSL_PARAM_BLD_push_int(bld, OSSL_PKEY_PARAM_USE_COFACTOR_ECDH, 1);
	if(Qxloc->len){
		CKINT(alloc_buf(Qxloc->len + Qyloc->len + 1, &publoc));
		publoc.buf[0]= POINT_CONVERSION_UNCOMPRESSED;
		memcpy(publoc.buf + 1, Qxloc->buf, Qxloc->len);
		memcpy(publoc.buf + 1 + Qxloc->len, Qyloc->buf, Qyloc->len);
		logger_binary(LOGGER_DEBUG, publoc.buf, publoc.len, "publoc");
		OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, publoc.buf,publoc.len);
	}
	if(privloc->len){
		privloc_bn = BN_bin2bn((const unsigned char *)privloc->buf, (int)privloc->len, NULL);
		OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, privloc_bn);
	}
	params = OSSL_PARAM_BLD_to_param(bld);
	CKNULL_LOG(params, -ENOMEM, "bld to param failed\n");
	kactx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	CKNULL_LOG(kactx, -ENOMEM, "EVP_PKEY_CTX_new_from_name failed\n");
	CKINT_O_LOG(EVP_PKEY_fromdata_init(kactx),
				"EVP_PKEY_fromdata_init failed with status=%d\n", ret);

	if(!(Qxloc->len) && !(privloc->len)) {
		pkey = EVP_PKEY_Q_keygen(NULL, NULL, "EC", digest);
		CKNULL_LOG(pkey, -EFAULT, "EVP_PKEY_Q_keygen failed\n");
	}
	else {
		CKINT_O_LOG(EVP_PKEY_fromdata(kactx, &pkey, EVP_PKEY_KEYPAIR, params),
					"EVP_PKEY_fromdata failed\n");
	}

	OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
			digest, strlen(digest)+1);

	CKINT(alloc_buf(Qxrem->len + Qyrem->len + 1, &pubrem));
	pubrem.buf[0]= POINT_CONVERSION_UNCOMPRESSED;
	memcpy(pubrem.buf + 1, Qxrem->buf, Qxrem->len);
	memcpy(pubrem.buf + 1 + Qxrem->len, Qyrem->buf, Qyrem->len);

	logger_binary(LOGGER_DEBUG, Qxrem->buf, Qxrem->len, "Qxrem");
	logger_binary(LOGGER_DEBUG, Qyrem->buf, Qyrem->len, "Qyrem");
	logger_binary(LOGGER_DEBUG, pubrem.buf, pubrem.len, "pubrem");

	OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
			pubrem.buf,pubrem.len);

	params_remote = OSSL_PARAM_BLD_to_param(bld);
	CKNULL_LOG(params_remote, -ENOMEM, "bld to param failed\n");
	kactx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	CKNULL_LOG(kactx, -ENOMEM, "EVP_PKEY_CTX_new_from_name failed\n");

	CKINT_O_LOG(EVP_PKEY_fromdata_init(kactx),
				"EVP_PKEY_fromdata_init failed\n");
	CKINT_O_LOG(EVP_PKEY_fromdata(kactx, &remotekey, EVP_PKEY_PUBLIC_KEY,
			params_remote), "EVP_PKEY_fromdata failed\n");
	dctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
	CKNULL_LOG(dctx, -ENOMEM, "EVP_PKEY_CTX_new_from_pkey failed\n");

	ret = EVP_PKEY_derive_init(dctx);
	if(ret <= 0) {
		logger(LOGGER_ERR, "EVP_PKEY_derive_init filed: %d\n", ret);
		goto out;
	}
	ret = EVP_PKEY_derive_set_peer(dctx, remotekey);
	if(ret <= 0) {
		logger(LOGGER_ERR, "EVP_PKEY_derive_set_peer filed: %d\n", ret);
		goto out;
	}
	ret = EVP_PKEY_derive(dctx, NULL, &ss.len);
	if(ret <= 0) {
		logger(LOGGER_ERR, "EVP_PKEY_derive filed: %d\n", ret);
		goto out;
	}
	CKINT(alloc_buf(ss.len, &ss));
	ret = EVP_PKEY_derive(dctx, ss.buf, &ss.len);
	if(ret <= 0) {
		logger(LOGGER_ERR, "EVP_PKEY_derive filed: %d\n", ret);
		goto out;
	}
	logger_binary(LOGGER_DEBUG, ss.buf, ss.len, "Generated shared secret");

	/* We do not use CKINT here, because -ENOENT is no real error */
	ret = openssl_hash_ss(cipher, &ss, hashzz);
out:
	if(pkey)
		EVP_PKEY_free(pkey);
	if(remotekey)
		EVP_PKEY_free(remotekey);
	if(kactx)
		EVP_PKEY_CTX_free(kactx);
	if(dctx)
		EVP_PKEY_CTX_free(dctx);
	if(params_remote)
		OSSL_PARAM_free(params_remote);
	if(params)
		OSSL_PARAM_free(params);
	if(bld)
		OSSL_PARAM_BLD_free(bld);
	if(privloc_bn)
		BN_free(privloc_bn);
	free_buf(&ss);
	free_buf(&publoc);
	free_buf(&pubrem);
	return ret;
}

static int openssl_ecdh_ss(struct ecdh_ss_data *data, flags_t parsed_flags)
{
	(void)parsed_flags;

	return openssl_ecdh_ss_common(data->cipher, &data->Qxrem, &data->Qyrem,
			&data->privloc,
			&data->Qxloc, &data->Qyloc,
			&data->hashzz);
}

static int openssl_ecdh_ss_ver(struct ecdh_ss_ver_data *data,
		flags_t parsed_flags)
{
	int ret = openssl_ecdh_ss_common(data->cipher, &data->Qxrem,
			&data->Qyrem,
			&data->privloc,
			&data->Qxloc, &data->Qyloc,
			&data->hashzz);

	(void)parsed_flags;

	if (ret == -EOPNOTSUPP || ret == -ENOENT) {
		data->validity_success = 0;
		logger(LOGGER_DEBUG, "ECDH validity test failed\n");
		return 0;
	} else if (!ret) {
		data->validity_success = 1;
		logger(LOGGER_DEBUG, "ECDH validity test passed\n");
		return 0;
	}
	logger(LOGGER_DEBUG, "ECDH validity test: general error\n");
	return ret;
}

static struct ecdh_backend openssl_ecdh =
{
	openssl_ecdh_ss,
	openssl_ecdh_ss_ver,
};

ACVP_DEFINE_CONSTRUCTOR(openssl_ecdh_backend)
static void openssl_ecdh_backend(void)
{
	register_ecdh_impl(&openssl_ecdh);
}

/************************************************
 * DRBG cipher interface functions
 ************************************************/
static int openssl_get_drbg_name(struct drbg_data *data, char *cipher,
		char *drbg_name)
{
	logger(LOGGER_DEBUG, "cipher: %" PRIu64 "\n", data->cipher);
	if ((data->cipher & ACVP_HASHMASK) == ACVP_SHA1) {
		strcpy(cipher, "SHA1");
		strcpy(drbg_name, ((data->type & ACVP_DRBGMASK) == ACVP_DRBGHMAC) ?
				"HMAC-DRBG" : "HASH-DRBG");
	} else if ((data->cipher & ACVP_HASHMASK) == ACVP_SHA224) {
		strcpy(cipher,  "SHA224");
		strcpy(drbg_name, ((data->type & ACVP_DRBGMASK) == ACVP_DRBGHMAC) ?
				"HMAC-DRBG" : "HASH-DRBG");
	} else if ((data->cipher & ACVP_HASHMASK) == ACVP_SHA256) {
		strcpy(cipher, "SHA256");
		strcpy(drbg_name, ((data->type & ACVP_DRBGMASK) == ACVP_DRBGHMAC) ?
				"HMAC-DRBG" : "HASH-DRBG");
	} else if ((data->cipher & ACVP_HASHMASK) == ACVP_SHA384) {
		strcpy(cipher,  "SHA384");
		strcpy(drbg_name, ((data->type & ACVP_DRBGMASK) == ACVP_DRBGHMAC) ?
				"HMAC-DRBG" : "HASH-DRBG");
	} else if ((data->cipher & ACVP_HASHMASK) == ACVP_SHA512224) {
		strcpy(cipher,  "SHA512-224");
		strcpy(drbg_name, ((data->type & ACVP_DRBGMASK) == ACVP_DRBGHMAC) ?
				"HMAC-DRBG" : "HASH-DRBG");
	} else if ((data->cipher & ACVP_HASHMASK) == ACVP_SHA512256) {
		strcpy(cipher, "SHA512-256");
		strcpy(drbg_name, ((data->type & ACVP_DRBGMASK) == ACVP_DRBGHMAC) ?
				"HMAC-DRBG" : "HASH-DRBG");
	} else if ((data->cipher & ACVP_HASHMASK) == ACVP_SHA512) {
		strcpy(cipher,  "SHA512");
		strcpy(drbg_name, ((data->type & ACVP_DRBGMASK) == ACVP_DRBGHMAC) ?
				"HMAC-DRBG" : "HASH-DRBG");
	} else if ((data->cipher & ACVP_AESMASK) == ACVP_AES128) {
		strcpy(cipher, "AES-128-CTR");
		strcpy(drbg_name, "CTR-DRBG");
	} else if ((data->cipher & ACVP_AESMASK) == ACVP_AES192) {
		strcpy(cipher, "AES-192-CTR");
		strcpy(drbg_name, "CTR-DRBG");
	} else if ((data->cipher & ACVP_AESMASK) == ACVP_AES256) {
		strcpy(cipher, "AES-256-CTR");
		strcpy(drbg_name, "CTR-DRBG");
	} else {
		logger(LOGGER_WARN, "DRBG with unhandled cipher detected\n");
		return -EFAULT;
	}
	return 0;
}

static int openssl_drbg_generate(struct drbg_data *data, flags_t parsed_flags)
{

	OSSL_PARAM params[4];
	char cipher[50];
	char drbg_name[50];
	EVP_RAND *rand = NULL;
	EVP_RAND_CTX *ctx = NULL, *parent = NULL;
	int df = 0;
	int ret = 0;
	unsigned int strength = 256;
	unsigned char *z;
	int res = 0;
	(void)parsed_flags;

	if (openssl_get_drbg_name(data, cipher, drbg_name) < 0)
		goto out;
	df = !!data->df;

	/* Create the seed source */
	rand = EVP_RAND_fetch(NULL, "TEST-RAND", "-fips");
	CKNULL(rand, -ENOMEM);
	parent = EVP_RAND_CTX_new(rand, NULL);
	CKNULL(parent, -ENOMEM);
	EVP_RAND_free(rand);
	rand = NULL;

	params[0] = OSSL_PARAM_construct_uint(OSSL_RAND_PARAM_STRENGTH, &strength);
	params[1] = OSSL_PARAM_construct_end();
	CKINT(EVP_RAND_CTX_set_params(parent, params));
	/* Get the DRBG */
	rand = EVP_RAND_fetch(NULL, drbg_name, NULL);
	CKNULL(rand, -ENOMEM);
	ctx = EVP_RAND_CTX_new(rand, parent);
	CKNULL(ctx, -ENOMEM);
	/* Set the DRBG up */
	strength = EVP_RAND_get_strength(ctx);
	params[0] = OSSL_PARAM_construct_int(OSSL_DRBG_PARAM_USE_DF,
			(int *)(&df));
	if(!strcmp(drbg_name,"CTR-DRBG")){
		params[1] = OSSL_PARAM_construct_utf8_string(OSSL_DRBG_PARAM_CIPHER,
				(char *)cipher, 0);
	}
	else {
		params[1] = OSSL_PARAM_construct_utf8_string(OSSL_DRBG_PARAM_DIGEST,
				(char *)cipher, strlen(cipher));
	}

	params[2] = OSSL_PARAM_construct_utf8_string(OSSL_DRBG_PARAM_MAC, "HMAC", 0);
	params[3] = OSSL_PARAM_construct_end();

	CKINT(EVP_RAND_CTX_set_params(ctx, params));
	/* Feed in the entropy and nonce */
	logger_binary(LOGGER_DEBUG, data->entropy.buf, data->entropy.len, "entropy");
	logger_binary(LOGGER_DEBUG, data->nonce.buf, data->nonce.len, "nonce");

	params[0] = OSSL_PARAM_construct_octet_string(OSSL_RAND_PARAM_TEST_ENTROPY,
			(void *)data->entropy.buf,
			data->entropy.len);
	params[1] = OSSL_PARAM_construct_octet_string(OSSL_RAND_PARAM_TEST_NONCE,
			(void *)data->nonce.buf,
			data->nonce.len);
	params[2] = OSSL_PARAM_construct_end();

	if (!EVP_RAND_instantiate(parent, strength, 0, NULL, 0, params)) {
		EVP_RAND_CTX_free(ctx);
		goto out;
	}
	/*
	 * Run the test
	 * A NULL personalisation string defaults to the built in so something
	 * non-NULL is needed if there is no personalisation string
	 */
	logger_binary(LOGGER_DEBUG, data->pers.buf, data->pers.len,
			"personalization string");

	z = data->pers.buf != NULL ? data->pers.buf : (unsigned char *)"";
	if (!EVP_RAND_instantiate(ctx, strength, data->pr, z, data->pers.len, NULL)) {
		logger(LOGGER_DEBUG, "DRBG instantiation failed: %s\n",
				ERR_error_string(ERR_get_error(), NULL));
		EVP_RAND_CTX_free(ctx);
		goto out;
	}

	if (data->entropy_reseed.buffers[0].len) {
		logger_binary(LOGGER_DEBUG,
				data->entropy_reseed.buffers[0].buf,
				data->entropy_reseed.buffers[0].len,
				"entropy reseed");

		params[0] = OSSL_PARAM_construct_octet_string
			(OSSL_RAND_PARAM_TEST_ENTROPY, data->entropy_reseed.buffers[0].buf,
			 data->entropy_reseed.buffers[0].len);
		params[1] = OSSL_PARAM_construct_end();
		CKINT(EVP_RAND_CTX_set_params(parent, params));
		if (data->addtl_reseed.buffers[0].len) {
			logger_binary(LOGGER_DEBUG,
					data->addtl_reseed.buffers[0].buf,
					data->addtl_reseed.buffers[0].len,
					"addtl reseed");
		}
		CKINT_O(EVP_RAND_reseed(ctx,data->pr,
					NULL, 0,
					data->addtl_reseed.buffers[0].buf,
					data->addtl_reseed.buffers[0].len));
	}
	if (data->entropy_generate.buffers[0].len) {
		logger_binary(LOGGER_DEBUG,
				data->entropy_generate.buffers[0].buf,
				data->entropy_generate.buffers[0].len,
				"entropy generate 1");
		params[0] = OSSL_PARAM_construct_octet_string
			(OSSL_RAND_PARAM_TEST_ENTROPY,
			 data->entropy_generate.buffers[0].buf,
			 data->entropy_generate.buffers[0].len);
		params[1] = OSSL_PARAM_construct_end();
		CKINT(EVP_RAND_CTX_set_params(parent, params));
	}

	logger_binary(LOGGER_DEBUG, data->addtl_generate.buffers[0].buf,
			data->addtl_generate.buffers[0].len, "addtl generate 1");
	CKINT(alloc_buf(data->rnd_data_bits_len / 8, &data->random));
	CKINT_O_LOG(EVP_RAND_generate(ctx, data->random.buf, data->random.len, strength,
				data->entropy_generate.buffers[0].len?1:0,
				data->addtl_generate.buffers[0].buf,
				data->addtl_generate.buffers[0].len),
			"FIPS_drbg_generate failed\n");
	logger_binary(LOGGER_DEBUG, data->random.buf, data->random.len,
			"random tmp");
	if (data->entropy_generate.buffers[1].len) {
		logger_binary(LOGGER_DEBUG, data->entropy_generate.buffers[1].buf,
				data->entropy_generate.buffers[1].len,
				"entropy generate 1");
		params[0] = OSSL_PARAM_construct_octet_string
			(OSSL_RAND_PARAM_TEST_ENTROPY,
			 data->entropy_generate.buffers[1].buf,
			 data->entropy_generate.buffers[1].len);
		params[1] = OSSL_PARAM_construct_end();
		CKINT(EVP_RAND_CTX_set_params(parent, params));
	}

	logger_binary(LOGGER_DEBUG, data->addtl_generate.buffers[1].buf,
			data->addtl_generate.buffers[1].len, "addtl generate 2");
	CKINT_O_LOG(EVP_RAND_generate(ctx, data->random.buf, data->random.len, strength,
				data->entropy_generate.buffers[1].len?1:0,
				data->addtl_generate.buffers[1].buf,
				data->addtl_generate.buffers[1].len),
			"FIPS_drbg_generate failed\n");
	logger_binary(LOGGER_DEBUG, data->random.buf, data->random.len,
			"random");

	/* Verify the output */
	res = 0;
out:
	if (ctx) {
		EVP_RAND_uninstantiate(ctx);
		EVP_RAND_CTX_free(ctx);
	}
	if(parent) {
		EVP_RAND_uninstantiate(parent);
		EVP_RAND_CTX_free(parent);
	}
	if(rand)
		EVP_RAND_free(rand);
	return res;
}

static struct drbg_backend openssl_drbg =
{
	openssl_drbg_generate,  /* drbg_generate */
};

ACVP_DEFINE_CONSTRUCTOR(openssl_drbg_backend)
static void openssl_drbg_backend(void)
{
	register_drbg_impl(&openssl_drbg);
}

/************************************************
 * TLS cipher interface functions
 ************************************************/

static int openssl_kdf_tls_op(struct kdf_tls_data *data, flags_t parsed_flags)
{
	EVP_PKEY_CTX *pctx = NULL;
	const EVP_MD *md;
	int ret;

	(void)parsed_flags;

	CKINT(openssl_md_convert(data->hashalg, &md));

	/* Special case */
	if ((data->hashalg & ACVP_HASHMASK) == ACVP_SHA1)
		md = EVP_get_digestbynid(NID_md5_sha1);

	CKNULL_LOG(md, -EFAULT, "Cipher implementation not found\n");

	CKINT(alloc_buf(data->pre_master_secret.len, &data->master_secret));

	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_TLS1_PRF, NULL);
	CKNULL_LOG(pctx, -EFAULT, "Cannot allocate TLS1 PRF\n");

	CKINT_O(EVP_PKEY_derive_init(pctx));
	CKINT_O(EVP_PKEY_CTX_set_tls1_prf_md(pctx, md));
	CKINT_O(EVP_PKEY_CTX_set1_tls1_prf_secret(pctx,
						  data->pre_master_secret.buf,
						  data->pre_master_secret.len));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx,
			(const unsigned char *)TLS_MD_MASTER_SECRET_CONST,
			TLS_MD_MASTER_SECRET_CONST_SIZE));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx,
						data->client_hello_random.buf,
						data->client_hello_random.len));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx, NULL, 0));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx,
						data->server_hello_random.buf,
						data->server_hello_random.len));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx, NULL, 0));
	CKINT_O(EVP_PKEY_derive(pctx, data->master_secret.buf,
				&data->master_secret.len));

	logger_binary(LOGGER_DEBUG, data->master_secret.buf,
		      data->master_secret.len, "master_secret");

	EVP_PKEY_CTX_free(pctx);
	pctx = NULL;

	CKINT(alloc_buf(data->key_block_length / 8, &data->key_block));

	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_TLS1_PRF, NULL);
	CKNULL_LOG(pctx, -EFAULT, "Cannot allocate TLS1 PRF\n");

	CKINT_O(EVP_PKEY_derive_init(pctx));
	CKINT_O(EVP_PKEY_CTX_set_tls1_prf_md(pctx, md));
	CKINT_O(EVP_PKEY_CTX_set1_tls1_prf_secret(pctx,
						  data->master_secret.buf,
						  data->master_secret.len));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx,
			(const unsigned char *)TLS_MD_KEY_EXPANSION_CONST,
			TLS_MD_KEY_EXPANSION_CONST_SIZE));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx,
						data->server_random.buf,
						data->server_random.len));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx,
						data->client_random.buf,
						data->client_random.len));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx, NULL, 0));
	CKINT_O(EVP_PKEY_CTX_add1_tls1_prf_seed(pctx, NULL, 0));
	CKINT_O(EVP_PKEY_derive(pctx, data->key_block.buf,
				&data->key_block.len));

	logger_binary(LOGGER_DEBUG, data->key_block.buf, data->key_block.len,
		      "keyblock");

	ret = 0;

out:
	if (pctx)
		EVP_PKEY_CTX_free(pctx);
	return (ret);
}

static struct kdf_tls_backend openssl_kdf_tls =
{
	openssl_kdf_tls_op,
};

ACVP_DEFINE_CONSTRUCTOR(openssl_kdf_tls_backend)
static void openssl_kdf_tls_backend(void)
{
	register_kdf_tls_impl(&openssl_kdf_tls);
}

/************************************************
 * SSHv2 KDF
 ************************************************/

static int openssl_kdf_ssh_internal(struct kdf_ssh_data *data,
				    int id, const EVP_MD *md,
				    struct buffer *out)
{
	EVP_KDF *kdf = NULL;
	EVP_KDF_CTX *ctx = NULL;
	OSSL_PARAM params[6], *p;
	int ret = 0;

	kdf = EVP_KDF_fetch(NULL, "SSHKDF", NULL);
	CKNULL_LOG(kdf, -EFAULT, "Cannot allocate SSHv2 KDF\n");
	ctx = EVP_KDF_CTX_new(kdf);
	CKNULL_LOG(ctx, -EFAULT, "Cannot allocate SSHv2 PRF\n");

	p = params;
	*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
						(char *)EVP_MD_name(md), 0);
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
						 data->k.buf, data->k.len);
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SSHKDF_XCGHASH,
						 data->h.buf, data->h.len);
	*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_SSHKDF_TYPE,
						(char *) &id, 0);
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SSHKDF_SESSION_ID,
						 data->session_id.buf,
						 data->session_id.len);
	*p = OSSL_PARAM_construct_end();

	CKINT_O(EVP_KDF_derive(ctx, out->buf, out->len, params));

out:
	if (kdf)
		EVP_KDF_free(kdf);
	if (ctx)
		EVP_KDF_CTX_free(ctx);
	return ret;
}

static int openssl_kdf_ssh(struct kdf_ssh_data *data, flags_t parsed_flags)
{
	const EVP_MD *md;
	unsigned int ivlen, enclen, maclen;
	int ret;

	(void)parsed_flags;

	CKINT(openssl_md_convert(data->cipher, &md));

	switch (data->cipher & ACVP_SYMMASK) {
	case ACVP_AES128:
		enclen = 16;
		ivlen = 16;
		break;
	case ACVP_AES192:
		enclen = 24;
		ivlen = 16;
		break;
	case ACVP_AES256:
		enclen = 32;
		ivlen = 16;
		break;
	case ACVP_TDESECB:
		enclen = 24;
		ivlen = 8;
		break;
	default:
		logger(LOGGER_WARN, "Cipher not identified\n");
		ret = -EINVAL;
		goto out;
	}

	switch (data->cipher & ACVP_HASHMASK) {
	case ACVP_SHA1:
		maclen = 20;
		break;
	case ACVP_SHA256:
		maclen = 32;
		break;
	case ACVP_SHA384:
		maclen = 48;
		break;
	case ACVP_SHA512:
		maclen = 64;
		break;
	default:
		logger(LOGGER_WARN, "Mac not identified\n");
		ret = -EINVAL;
		goto out;
	}

	CKINT(alloc_buf(ivlen, &data->initial_iv_client));
	CKINT(alloc_buf(ivlen, &data->initial_iv_server));
	CKINT(alloc_buf(enclen, &data->encryption_key_client));
	CKINT(alloc_buf(enclen, &data->encryption_key_server));
	CKINT(alloc_buf(maclen, &data->integrity_key_client));
	CKINT(alloc_buf(maclen, &data->integrity_key_server));

	CKINT(openssl_kdf_ssh_internal(data,  'A' + 0, md,
				       &data->initial_iv_client));
	CKINT(openssl_kdf_ssh_internal(data,  'A' + 1, md,
				       &data->initial_iv_server));
	CKINT(openssl_kdf_ssh_internal(data,  'A' + 2, md,
				       &data->encryption_key_client));
	CKINT(openssl_kdf_ssh_internal(data,  'A' + 3, md,
				       &data->encryption_key_server));
	CKINT(openssl_kdf_ssh_internal(data,  'A' + 4, md,
				       &data->integrity_key_client));
	CKINT(openssl_kdf_ssh_internal(data,  'A' + 5, md,
				       &data->integrity_key_server));

out:
	return ret;
}

static struct kdf_ssh_backend openssl_kdf =
{
	openssl_kdf_ssh,
};

ACVP_DEFINE_CONSTRUCTOR(openssl_kdf_ssh_backend)
static void openssl_kdf_ssh_backend(void)
{
	register_kdf_ssh_impl(&openssl_kdf);
}

#ifdef OPENSSL_KBKDF
/************************************************
 * SP 800-108 KBKDF interface functions
 ************************************************/

static int openssl_kdf108(struct kdf_108_data *data, flags_t parsed_flags)
{
	EVP_KDF *kdf = NULL;
	EVP_KDF_CTX *ctx = NULL;
	OSSL_PARAM params[8], *p;
	const EVP_MD *md = NULL;
	const EVP_CIPHER *type = NULL;
	uint32_t derived_key_bytes = data->derived_key_length / 8;
	uint32_t l = be32(data->derived_key_length);
	BUFFER_INIT(label);
	BUFFER_INIT(context);
	int ret = 0, alloced = 0;
	(void)parsed_flags;

	logger(LOGGER_VERBOSE, "data->kdfmode = %" PRIu64 "\n", data->kdfmode);
	if (!(data->kdfmode & ACVP_CIPHERTYPE_KDF)) {
		logger(LOGGER_ERR, "The cipher type isn't a KDF");
		ret = -EINVAL;
		goto out;
	}

	if (data->kdfmode == ACVP_KDF_108_DOUBLE_PIPELINE) {
		logger(LOGGER_ERR, "Double pipeline mode is not supported");
		ret = -EINVAL;
		goto out;
	}

	kdf = EVP_KDF_fetch(NULL, "KBKDF", NULL);
	CKNULL_LOG(kdf, -EFAULT, "Cannot allocate KB KDF\n");
	ctx = EVP_KDF_CTX_new(kdf);
	CKNULL_LOG(ctx, -EFAULT, "Cannot allocate KB PRF\n");


	p = params;
	*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_MODE,
				(data->kdfmode == ACVP_KDF_108_COUNTER) ?
				"counter" : "feedback", 0);

	logger(LOGGER_VERBOSE, "data->mac = %" PRIu64 "\n", data->mac);
	if (data->mac & ACVP_CIPHERTYPE_HMAC) {
		CKINT(openssl_md_convert(data->mac, &md));
		CKNULL(md, -ENOMEM);

		*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
						(char *)EVP_MD_name(md), 0);
		*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_MAC,
							"HMAC", 0);
	} else if (data->mac & ACVP_CIPHERTYPE_CMAC) {
		CKINT(openssl_cipher(data->mac == ACVP_AESCMAC ? ACVP_AESCMAC :
				     ACVP_TDESCMAC, data->key.len, &type));
		CKNULL(type, -ENOMEM);

		*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_CIPHER,
						(char *)EVP_CIPHER_name(type),
						0);
		*p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_MAC,
							"CMAC", 0);
	}

	logger_binary(LOGGER_VERBOSE, data->key.buf, data->key.len, "data->key");
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
						 data->key.buf, data->key.len);

	logger(LOGGER_VERBOSE, "L = %u\n", derived_key_bytes);
	logger_binary(LOGGER_VERBOSE, (unsigned char *)&l, sizeof(l), "[L]_2");

	if (data->fixed_data.len) {
		if (data->fixed_data.len != (data->key.len * 2 + 1 + sizeof(l))) {
			logger(LOGGER_ERR, "KBKDF fixed data unexpected length for regression testing\n");
			ret = -EINVAL;
			goto out;
		}
		label.buf = data->fixed_data.buf;
		label.len = data->key.len;
		context.buf = data->fixed_data.buf + 1 + label.len;
		context.len = data->key.len;
	} else {
		alloced = 1;

		CKINT(alloc_buf(data->key.len, &label));
		CKINT(alloc_buf(data->key.len, &context));
		/*
		 * Allocate the fixed_data to hold
		 * Label || 0x00 || Context || [L]_2
		 */
		CKINT(alloc_buf(label.len + 1 + context.len + sizeof(l),
			&data->fixed_data));

		/* Randomly choose the label and context */
		RAND_bytes(label.buf, (int)label.len);
		RAND_bytes(context.buf, (int)context.len);

		/*
		 * Fixed data = Label || 0x00 || Context || [L]_2
		 * The counter i is not part of it
		 */
		memcpy(data->fixed_data.buf, label.buf, label.len);
		       data->fixed_data.buf[label.len] = 0x00;
		memcpy(data->fixed_data.buf + label.len + 1, context.buf,
		       context.len);
		memcpy(data->fixed_data.buf + label.len + 1 + context.len,
		       (unsigned char *)&l, sizeof(l));

		logger_binary(LOGGER_VERBOSE, data->fixed_data.buf,
			      data->fixed_data.len, "data->fixed_data");
	}

	logger_binary(LOGGER_VERBOSE, label.buf, label.len, "label");
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, label.buf,
						 label.len);

	logger_binary(LOGGER_VERBOSE, context.buf, context.len, "context");
	*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
						 context.buf, context.len);

	if (data->iv.len) {
		logger_binary(LOGGER_VERBOSE, data->iv.buf, data->iv.len,
			      "data->iv");
		*p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SEED,
							 data->iv.buf,
							 data->iv.len);
	}

	*p = OSSL_PARAM_construct_end();

	CKINT(alloc_buf(derived_key_bytes, &data->derived_key));
	CKINT_O_LOG(EVP_KDF_derive(ctx, data->derived_key.buf,
				   derived_key_bytes, params),
		    "EVP_KDF_derive failed\n");
	logger_binary(LOGGER_VERBOSE, data->derived_key.buf,
                      derived_key_bytes, "data->derived_key");

out:
	if (kdf)
		EVP_KDF_free(kdf);
	if (ctx)
		EVP_KDF_CTX_free(ctx);

	if (alloced) {
		free_buf(&label);
		free_buf(&context);
	}
	return ret;
}

static struct kdf_108_backend openssl_kdf108_backend =
{
	openssl_kdf108,
};

ACVP_DEFINE_CONSTRUCTOR(openssl_kdf_108_backend)
static void openssl_kdf_108_backend(void)
{
	register_kdf_108_impl(&openssl_kdf108_backend);
}
#endif
