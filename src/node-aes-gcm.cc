// The MIT License (MIT)
// 
// Copyright (c) 2013 Patrick Van Oosterwijck
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
 
#include <node.h>
#include <nan.h>
#include <openssl/evp.h>

#include "node-aes-gcm.h"

// see https://wiki.openssl.org/index.php/EVP_Authenticated_Encryption_and_Decryption
// for details on the implementation

using namespace v8;
using namespace node;


// Authentication tag length

#define AUTH_TAG_LEN              16

// Different versions of OpenSSL use different IV length defines

#ifndef EVP_CTRL_GCM_SET_IVLEN
#define EVP_CTRL_GCM_SET_IVLEN    EVP_CTRL_AEAD_SET_IVLEN
#endif


// Perform GCM mode AES encryption using the
// provided key, IV, plaintext and auth_data buffers, and return an object
// containing "ciphertext" and "auth_tag" buffers.
// The key length determines the encryption bit level used.
NAN_METHOD(gcm::Encrypt) {
	Nan::HandleScope scope;

	// check arguments
	if (info.Length() < 4 || 
		!Buffer::HasInstance(info[0]) || // key
		!Buffer::HasInstance(info[1]) || // iv
		!Buffer::HasInstance(info[2]) || // plaintext
		!(info[3]->IsUndefined() || info[3]->IsNull() || Buffer::HasInstance(info[3])) // auth_data, optional
	) {
		Nan::ThrowError(
			"Not enough (or wrong) arguments specified. Required: "
			"key (Buffer), iv (Buffer), plaintext (Buffer), auth_data (Buffer | NULL)."
		);
		return;
	}

	// parse key and assign a cipher
	const EVP_CIPHER *cipher_type = NULL;
	unsigned char *key = (unsigned char *)Buffer::Data(info[0]);
	size_t key_len = Buffer::Length(info[0]);
	switch (key_len) {
		case 16:
			cipher_type = EVP_aes_128_gcm();
			break;
		case 24:
			cipher_type = EVP_aes_192_gcm();
			break;
		case 32:
			cipher_type = EVP_aes_256_gcm();
			break;
		default:
			Nan::ThrowError("Invalid key length specified. Allowed are 128, 192 and 256 bits.");
			return;
	}

	// parse iv and plaintext
	unsigned char *iv = (unsigned char *)Buffer::Data(info[1]);
	const size_t iv_len = Buffer::Length(info[1]);
	unsigned char *plaintext = (unsigned char *)Buffer::Data(info[2]);
	const size_t plaintext_len = Buffer::Length(info[2]);
	// Make a buffer for the ciphertext that is the same size as the
	// plaintext, but padded to 16 byte increments
	size_t ciphertext_len = (((plaintext_len - 1) / key_len) + 1) * key_len;
	unsigned char *ciphertext = new unsigned char[ciphertext_len];
	// parse auth data (if given)
	const bool hasAuthData = Buffer::HasInstance(info[3]);
	unsigned char *aad;
	size_t aad_len = 0;
	if (hasAuthData) {
		aad = (unsigned char *)Buffer::Data(info[3]);
		aad_len = Buffer::Length(info[3]);
	}
	// Make a authentication tag buffer
	unsigned char *auth_tag = new unsigned char[AUTH_TAG_LEN];
	
		
	// ==================

	// Now do the encryption

	// create the context
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	// initialize the encryption operation with the chosen cipher
	EVP_EncryptInit_ex(ctx, cipher_type, NULL, NULL, NULL);

	// set iv and auth tag length
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL);

	// provide the key and iv
	EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv);
	
	int outl; // output length
	
	// if we have additional authenticated data, provide it
	if (hasAuthData) {
		EVP_EncryptUpdate(ctx, NULL, &outl, aad, aad_len);
	}
	
	// Encrypt plaintext
	EVP_EncryptUpdate(ctx, ciphertext, &outl, plaintext, plaintext_len);
	ciphertext_len = outl;

	// Finalize the encryption
	EVP_EncryptFinal_ex(ctx, ciphertext + outl, &outl);
	ciphertext_len += outl;

	// Get the authentication tag
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, AUTH_TAG_LEN, auth_tag);

	// Clean up
	EVP_CIPHER_CTX_free(ctx);
	
	// ===================================
	
	// Create the return buffers and object
	// We strip padding from the ciphertext
	Nan::MaybeLocal<Object> ciphertext_buf = Nan::CopyBuffer((char*)ciphertext, (uint32_t)ciphertext_len);
	Nan::MaybeLocal<Object> auth_tag_buf = Nan::CopyBuffer((char*)auth_tag, AUTH_TAG_LEN);
	Local<Object> return_obj = Nan::New<Object>();
	Nan::Set(return_obj, Nan::New<String>("ciphertext").ToLocalChecked(), ciphertext_buf.ToLocalChecked());
	Nan::Set(return_obj, Nan::New<String>("auth_tag").ToLocalChecked(), auth_tag_buf.ToLocalChecked());

	// Return it
	info.GetReturnValue().Set(return_obj);
}

// Perform GCM mode AES decryption using the
// provided key, IV, ciphertext, auth_data and auth_tag buffers, and return
// an object containing a "plaintext" buffer and an "auth_ok" boolean.
// The key length determines the encryption bit level used.

NAN_METHOD(gcm::Decrypt) {
	Nan::HandleScope scope;

	// check arguments
	if (info.Length() < 5 || 
		!Buffer::HasInstance(info[0]) || // key
		!Buffer::HasInstance(info[1]) || // iv
		!Buffer::HasInstance(info[2]) || // ciphertext
		!(info[3]->IsUndefined() || info[3]->IsNull() || Buffer::HasInstance(info[3])) || // auth_data, optional
		!Buffer::HasInstance(info[4]) || // auth tag
		Buffer::Length(info[4]) != AUTH_TAG_LEN
	) {
		Nan::ThrowError(
			"Not enough (or wrong) arguments specified. Required: "
			"key (Buffer), iv (Buffer), ciphertext (Buffer), auth_data (Buffer | NULL), auth tag (Buffer, 16 bytes)."
		);
		return;
	}
	
	// parse key and assign a cipher
	const EVP_CIPHER *cipher_type = NULL;
	unsigned char *key = (unsigned char *)Buffer::Data(info[0]);
	size_t key_len = Buffer::Length(info[0]);
	switch (key_len) {
		case 16:
			cipher_type = EVP_aes_128_gcm();
			break;
		case 24:
			cipher_type = EVP_aes_192_gcm();
			break;
		case 32:
			cipher_type = EVP_aes_256_gcm();
			break;
		default:
			Nan::ThrowError("Invalid key length specified. Allowed are 128, 192 and 256 bits.");
			return;
	}

	// parse iv and ciphertext
	unsigned char *iv = (unsigned char *)Buffer::Data(info[1]);
	const size_t iv_len = Buffer::Length(info[1]);
	unsigned char *ciphertext = (unsigned char *)Buffer::Data(info[2]);
	const size_t ciphertext_len = Buffer::Length(info[2]);
	// Make a buffer for the plaintext that is the same size as the
	// ciphertext, but padded to 16 byte increments
	size_t plaintext_len = (((ciphertext_len - 1) / 16) + 1) * 16;
	unsigned char *plaintext = new unsigned char[plaintext_len];
	// parse auth data (if given)
	const bool hasAuthData = Buffer::HasInstance(info[3]);
	unsigned char *aad;
	size_t aad_len = 0;
	if (hasAuthData) {
		aad = (unsigned char *)Buffer::Data(info[3]);
		aad_len = Buffer::Length(info[3]);
	}
	// parse auth_tag
	unsigned char *auth_tag = (unsigned char *)Buffer::Data(info[4]);


	// ==================

	// Now do the decryption

	// create the context
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	// initialize the decryption operation with the chosen cipher
	EVP_DecryptInit_ex(ctx, cipher_type, NULL, NULL, NULL);

	// Set the IV length
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL);
	
	// Provide key and iv to OpenSSL
	EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv);
	
	int outl; // output length
	
	// if we have additional authenticated data, provide it
	if (hasAuthData) {
		EVP_DecryptUpdate(ctx, NULL, &outl, aad, aad_len);
	}

	// Decrypt ciphertext
	EVP_DecryptUpdate(ctx, plaintext, &outl, ciphertext, ciphertext_len);
	plaintext_len = outl;

	// Set the input reference authentication tag
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, AUTH_TAG_LEN, auth_tag);

	// Finalize
	bool auth_ok = EVP_DecryptFinal_ex(ctx, plaintext + outl, &outl);
	plaintext_len += outl;

	// Clean up
	EVP_CIPHER_CTX_free(ctx);
	
	// ==================

	// Create the return buffer and object
	// We strip padding from the plaintext
	Nan::MaybeLocal<Object> plaintext_buf = Nan::CopyBuffer((char*)plaintext, (uint32_t)plaintext_len);
	Local<Object> return_obj = Nan::New<Object>();
	Nan::Set(return_obj, Nan::New<String>("plaintext").ToLocalChecked(), plaintext_buf.ToLocalChecked());
	Nan::Set(return_obj, Nan::New<String>("auth_ok").ToLocalChecked(), Nan::New<Boolean>(auth_ok));

	// Return it
	info.GetReturnValue().Set(return_obj);
}