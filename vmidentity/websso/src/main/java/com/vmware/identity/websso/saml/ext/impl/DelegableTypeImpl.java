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
package com.vmware.identity.websso.saml.ext.impl;

import java.util.Collections;
import java.util.List;

import org.opensaml.common.impl.AbstractSAMLObject;
import org.opensaml.xml.XMLObject;

import com.vmware.identity.websso.saml.ext.DelegableType;

/**
 * DelegableType implementation
 *
 */
public final class DelegableTypeImpl extends AbstractSAMLObject implements
        DelegableType {

    DelegableTypeImpl(String namespaceURI, String elementLocalName,
            String namespacePrefix) {
            super(namespaceURI, elementLocalName, namespacePrefix);
         }

    /* (non-Javadoc)
     * @see org.opensaml.xml.XMLObject#getOrderedChildren()
     */
    public List<XMLObject> getOrderedChildren() {
        return Collections.emptyList();
    }

}