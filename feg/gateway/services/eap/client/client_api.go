/*
Copyright (c) Facebook, Inc. and its affiliates.
All rights reserved.

This source code is licensed under the BSD-style license found in the
LICENSE file in the root directory of this source tree.
*/

// Package client (eap.client) provides interface to supported & registered EAP Authenticator Providers
//
package client

import (
	"errors"
	"fmt"

	"magma/feg/gateway/services/eap"
	"magma/feg/gateway/services/eap/protos"
	"magma/feg/gateway/services/eap/providers/registry"
)

const (
	// EAP Related Consts
	EapMethodIdentity = uint8(protos.EapType_Identity)
	EapCodeResponse   = uint8(protos.EapCode_Response)
)

// HandleIdentityResponse passes Identity EAP payload to corresponding method provider & returns corresponding
// EAP result
// NOTE: Identity Request is handled by APs & does not involve EAP Authenticator's support
func HandleIdentityResponse(providerType uint8, msg *protos.Eap) (*protos.Eap, error) {
	if msg == nil {
		return nil, errors.New("Nil EAP Request")
	}
	err := verifyEapPayload(msg.Payload)
	if err != nil {
		return nil, err
	}
	if msg.Payload[eap.EapMsgMethodType] != EapMethodIdentity {
		return nil, fmt.Errorf(
			"Invalid EAP Method Type for Identity Response: %d. Expecting EAP Identity (%d)",
			msg.Payload[eap.EapMsgMethodType], EapMethodIdentity)
	}
	p := registry.GetProvider(providerType)
	if p == nil {
		return nil, unsupportedProviderError(providerType)
	}
	return p.Handle(&protos.Eap{Payload: msg.Payload})
}

// SupportedTypes returns sorted list (ascending, by type) of registered EAP Providers
// SupportedTypes makes copy of an internally maintained supported types list, so callers
// are advised to save the result locally and re-use it if needed
func SupportedTypes() []uint8 {
	return registry.SupportedTypes()
}

// Handle handles passed EAP payload & returns corresponding EAP result
func Handle(msg *protos.Eap) (*protos.Eap, error) {
	if msg == nil {
		return nil, errors.New("Nil EAP Message")
	}
	err := verifyEapPayload(msg.Payload)
	if err != nil {
		return nil, err
	}
	p := registry.GetProvider(msg.Payload[eap.EapMsgMethodType])
	if p == nil {
		return nil, unsupportedProviderError(msg.Payload[eap.EapMsgMethodType])
	}
	return p.Handle(msg)
}

// verifyEapPayload checks validity of EAP message & it's length
func verifyEapPayload(payload []byte) error {
	el := len(payload)
	if el < eap.EapMsgData {
		return fmt.Errorf("EAP Message is too short: %d bytes", el)
	}
	mLen := uint16(payload[eap.EapMsgLenHigh])<<8 + uint16(payload[eap.EapMsgLenLow])
	if el < int(mLen) {
		return fmt.Errorf("Invalid EAP Message: bytes received %d are below specified length %d", el, mLen)
	}
	if payload[eap.EapMsgCode] != EapCodeResponse {
		return fmt.Errorf(
			"Unsupported EAP Code: %d. Expecting EAP-Response (%d)",
			payload[eap.EapMsgCode], EapCodeResponse)
	}
	return nil
}

func unsupportedProviderError(methodType uint8) error {
	return fmt.Errorf("Unsupported EAP Provider for Method Type: %d", methodType)
}
