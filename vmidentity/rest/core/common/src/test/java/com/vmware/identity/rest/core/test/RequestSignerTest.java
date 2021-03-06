/*
 *  Copyright (c) 2012-2015 VMware, Inc.  All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not
 *  use this file except in compliance with the License.  You may obtain a copy
 *  of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, without
 *  warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 *  License for the specific language governing permissions and limitations
 *  under the License.
 */
package com.vmware.identity.rest.core.test;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.net.URI;
import java.net.URISyntaxException;
import java.security.InvalidKeyException;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.NoSuchAlgorithmException;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.SignatureException;
import java.util.Date;

import org.apache.commons.codec.DecoderException;
import org.junit.BeforeClass;
import org.junit.Test;

import com.vmware.identity.rest.core.util.RequestSigner;

public class RequestSignerTest {

    private static PublicKey publicKey;
    private static PrivateKey privateKey;
    private static final String SHA256 = "SHA-256";
    private static final String MD5 = "MD5";

    @BeforeClass
    public static void setup() throws NoSuchAlgorithmException {
        KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA");
        generator.initialize(2048);

        KeyPair pair = generator.generateKeyPair();
        publicKey = pair.getPublic();
        privateKey = pair.getPrivate();
    }

    @Test
    public void testSigning() throws NoSuchAlgorithmException, URISyntaxException, InvalidKeyException, SignatureException, DecoderException {
        String sha256 = RequestSigner.computeEntityHash("this is an entity", SHA256);
        Date date = new Date();
        URI uri = new URI("http://localhost/");

        String stringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, uri);

        String signedRequestHex = RequestSigner.sign(stringToSign, privateKey);

        assertTrue("Signature was not verified", RequestSigner.verify(signedRequestHex, stringToSign, publicKey));
    }

    @Test
    public void testSigning_BadURI() throws NoSuchAlgorithmException, URISyntaxException, InvalidKeyException, SignatureException, DecoderException {
        String sha256 = RequestSigner.computeEntityHash("this is an entity", SHA256);
        Date date = new Date();
        URI uri = new URI("http://localhost/path/to/data");
        URI badUri = new URI("http://junkhost/not/the/path");

        String stringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, uri);
        String badStringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, badUri);

        String signedRequestHex = RequestSigner.sign(stringToSign, privateKey);

        assertFalse("Signature was verified", RequestSigner.verify(signedRequestHex, badStringToSign, publicKey));
    }

    @Test
    public void testSigning_BadDate() throws NoSuchAlgorithmException, URISyntaxException, InvalidKeyException, SignatureException, DecoderException {
        String sha256 = RequestSigner.computeEntityHash("this is an entity", SHA256);;
        Date date = new Date();
        Date badDate = new Date(date.getTime() + 1000);
        URI uri = new URI("http://localhost/");

        String stringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, uri);
        String badStringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", badDate, uri);

        String signedRequestHex = RequestSigner.sign(stringToSign, privateKey);

        assertFalse("Signature was verified", RequestSigner.verify(signedRequestHex, badStringToSign, publicKey));
    }

    @Test
    public void testSigning_BadContentType() throws NoSuchAlgorithmException, URISyntaxException, InvalidKeyException, SignatureException, DecoderException {
        String sha256 = RequestSigner.computeEntityHash("this is an entity", SHA256);
        Date date = new Date();
        URI uri = new URI("http://localhost/");

        String stringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, uri);
        String badStringToSign = RequestSigner.createSigningString("GET", sha256, "application/xml", date, uri);

        String signedRequestHex = RequestSigner.sign(stringToSign, privateKey);

        assertFalse("Signature was verified", RequestSigner.verify(signedRequestHex, badStringToSign, publicKey));
    }

    @Test
    public void testSigning_BadSHA256() throws NoSuchAlgorithmException, URISyntaxException, InvalidKeyException, SignatureException, DecoderException {
        String sha256 = RequestSigner.computeEntityHash("this is an entity", SHA256);
        String badSha256 = RequestSigner.computeEntityHash("bad entity", SHA256);

        Date date = new Date();
        URI uri = new URI("http://localhost/");

        String stringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, uri);
        String badStringToSign = RequestSigner.createSigningString("GET", badSha256, "application/json; charset=UTF-8", date, uri);

        String signedRequestHex = RequestSigner.sign(stringToSign, privateKey);

        assertFalse("Signature was verified", RequestSigner.verify(signedRequestHex, badStringToSign, publicKey));
    }

    @Test
    public void testSigning_WrongAlgorithm() throws URISyntaxException, InvalidKeyException, SignatureException, DecoderException {
        String sha256 = RequestSigner.computeEntityHash("this is an entity", SHA256);
        String wrongAlg = RequestSigner.computeEntityHash("this is an entity", MD5);

        Date date = new Date();
        URI uri = new URI("http://localhost/");

        String stringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, uri);
        String badStringToSign = RequestSigner.createSigningString("GET", wrongAlg, "application/json; charset=UTF-8", date, uri);

        String signedRequestHex = RequestSigner.sign(stringToSign, privateKey);

        assertFalse("Signature was verified", RequestSigner.verify(signedRequestHex, badStringToSign, publicKey));
    }

    @Test
    public void testSigning_BadMethod() throws NoSuchAlgorithmException, URISyntaxException, InvalidKeyException, SignatureException, DecoderException {
        String sha256 = RequestSigner.computeEntityHash("this is an entity", SHA256);
        Date date = new Date();
        URI uri = new URI("http://localhost/");

        String stringToSign = RequestSigner.createSigningString("GET", sha256, "application/json; charset=UTF-8", date, uri);
        String badStringToSign = RequestSigner.createSigningString("POST", sha256, "application/json; charset=UTF-8", date, uri);

        String signedRequestHex = RequestSigner.sign(stringToSign, privateKey);

        assertFalse("Signature was verified", RequestSigner.verify(signedRequestHex, badStringToSign, publicKey));
    }

}
