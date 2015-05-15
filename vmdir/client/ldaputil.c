/*
 * Copyright © 2012-2015 VMware, Inc.  All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the “License”); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS, without
 * warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */



#include "includes.h"

extern PCSTR gGroupWhiteList[];

static
DWORD
_VmDirLdapSetupAccountMembership(
    LDAP*   pLd,
    PCSTR   pszDomainDN,
    PCSTR   pszBuiltinGroupName,
    PCSTR   pszAccountDN
    );

/*
 * Since there is no ldap_count_attributes api in openLDAP,
 * I created it.
 * Follow ldap api naming convention
 * Used in VmDirLdapCopyEntry
 */
static
DWORD
ldap_count_attributes(
    LDAP* pLd,
    LDAPMessage* pEntry
    )
{
    int         count = 0;
    BerElement* ber = NULL;
    PSTR        attr = NULL;

    for (attr = ldap_first_attribute(pLd, pEntry, &ber);
         attr != NULL;
         attr = ldap_next_attribute(pLd, pEntry, ber))
    {
        count++;
        ldap_memfree(attr);
    }

    if (ber)
    {
        ber_free(ber,0);
    }

    return count;
}

/*
 * Copy an entry from source to target.
 * Used in VmDirCopyLDAPSubTree
 */
static
DWORD
VmDirLdapCopyEntry(
    LDAP *pLdSource,
    LDAP *pLdTarget,
    LDAPMessage* entry,
    PCSTR pszDN,
    PBYTE pOldMasterKey,
    DWORD dwOldMasterKeyLen,
    PBYTE pNewMasterKey,
    DWORD dwNewMasterKeyLen
    )
{
    DWORD       dwError = 0;
    BerElement* ber = NULL;
    int         count = ldap_count_attributes(pLdSource, entry);
    LDAPMod**   ppAttrs = NULL;
    PSTR        attr = NULL;
    BerValue    bv_default = {
                        strlen(PASSWD_SCHEME_VMDIRD),
                        PASSWD_SCHEME_VMDIRD};
    BerVarray   bervals[]={&bv_default, NULL};
    int         i = 0;

    if (pLdSource == NULL || pLdTarget == NULL || entry == NULL || pszDN == NULL)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }
    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Dn: %s",pszDN);

    dwError = VmDirAllocateMemory((count+2)*sizeof(LDAPMod*), (PVOID*)&ppAttrs);
    BAIL_ON_VMDIR_ERROR(dwError);

    for (i=0, attr = ldap_first_attribute(pLdSource, entry, &ber);
         i<count && attr;
         i++, attr = ldap_next_attribute(pLdSource, entry, ber))
    {
        if (VmDirStringCompareA(attr, ATTR_USER_PASSWORD, FALSE) == 0)
        {
            dwError = VmDirAllocateMemory(sizeof(LDAPMod), (PVOID*)&ppAttrs[count]);
            BAIL_ON_VMDIR_ERROR(dwError);

            ppAttrs[count]->mod_op = LDAP_MOD_ADD|LDAP_MOD_BVALUES;
            ppAttrs[count]->mod_type = ATTR_PASSWORD_SCHEME;
            ppAttrs[count]->mod_vals.modv_bvals = bervals;
        }

        dwError = VmDirAllocateMemory(sizeof(LDAPMod), (PVOID*)&ppAttrs[i]);
        BAIL_ON_VMDIR_ERROR(dwError);

        ppAttrs[i]->mod_op = LDAP_MOD_ADD|LDAP_MOD_BVALUES;
        ppAttrs[i]->mod_type = attr;
        if (VmDirStringCompareA(attr, ATTR_KRB_PRINCIPAL_KEY, FALSE)) //not krb key
        {
            ppAttrs[i]->mod_vals.modv_bvals = ldap_get_values_len(pLdSource, entry, attr);
        }
        else //for krb key
        {
            BerValue** userKey = NULL;
            BerValue** values = NULL;
            dwError = VmDirAllocateMemory(sizeof(BerValue*)*2, (PVOID*)&userKey);
            BAIL_ON_VMDIR_ERROR(dwError);
            dwError = VmDirAllocateMemory(sizeof(BerValue), (PVOID*)&userKey[0]);
            BAIL_ON_VMDIR_ERROR(dwError);

            values = ldap_get_values_len(pLdSource, entry, attr);

            dwError = VmDirMigrateKrbUPNKey(
                            values[0]->bv_val,
                            (DWORD)values[0]->bv_len,
                            pOldMasterKey,
                            dwOldMasterKeyLen,
                            pNewMasterKey,
                            dwNewMasterKeyLen,
                            (PVOID)&userKey[0]->bv_val,
                            (PVOID)&userKey[0]->bv_len);
            ldap_value_free_len(values);
            ppAttrs[i]->mod_vals.modv_bvals = userKey;
            BAIL_ON_VMDIR_ERROR(dwError);
        }
        attr = NULL;
    }
    dwError = ldap_add_ext_s(
                             pLdTarget,
                             pszDN,
                             ppAttrs,
                             NULL,
                             NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    if (ber)
    {
        ber_free(ber,0);
    }

    for (i=0; i<count; i++)
    {
        if (ppAttrs[i])
        {
            if (ppAttrs[i]->mod_vals.modv_bvals)
            {
                if (VmDirStringCompareA(ppAttrs[i]->mod_type, ATTR_KRB_PRINCIPAL_KEY, FALSE)) //not krb key
                {
                    ldap_value_free_len(ppAttrs[i]->mod_vals.modv_bvals);
                }
                else
                {
                    VMDIR_SAFE_FREE_MEMORY(ppAttrs[i]->mod_vals.modv_bvals[0]->bv_val);
                    VMDIR_SAFE_FREE_MEMORY(ppAttrs[i]->mod_vals.modv_bvals[0]);
                    VMDIR_SAFE_FREE_MEMORY(ppAttrs[i]->mod_vals.modv_bvals);
                }
            }

            if (ppAttrs[i]->mod_type)
            {
                ldap_memfree(ppAttrs[i]->mod_type);
            }

            VMDIR_SAFE_FREE_MEMORY(ppAttrs[i]);
        }
    }

    VMDIR_SAFE_FREE_MEMORY(ppAttrs);

    return dwError;

error:
    if (attr)
    {
        ldap_memfree(attr);
    }

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapCopyEntry failed. Error(%u)", dwError);

    goto cleanup;
}

/*
 * Create dn for remote repl hostname using ldap search lookup
 * Sample of remote repl host DN:
 * cn=blisles-B,cn=Servers,cn=siteB,cn=Sites,cn=Configuration,dc=vmware,dc=com
 *
 */
DWORD
VmDirLdapCreateReplHostNameDN(
    PSTR* ppszReplHostNameDN,
    LDAP* pLd,
    PCSTR pszReplHostName
    )
{
    DWORD dwError = 0;
    PSTR pszReplHostNameDN = NULL;
    PSTR pszSearchFilter = NULL;
    LDAPMessage* searchRes = NULL;

    LDAPMessage* entry = NULL;
    PSTR pszDn = NULL;

    dwError = VmDirAllocateStringAVsnprintf(
                  &pszSearchFilter,
                  "(&(cn=%s)(objectclass=vmwDirServer))",
                  pszReplHostName
                  );
    dwError = ldap_search_ext_s( pLd,
                                 "",
                                 LDAP_SCOPE_SUBTREE,
                                 pszSearchFilter,
                                 NULL, TRUE,
                                 NULL,                       // server ctrls
                                 NULL,                       // client ctrls
                                 NULL,                       // timeout
                                 0,                         // size limit,
                                 &searchRes
                               );
    BAIL_ON_VMDIR_ERROR(dwError);

    if (1 != ldap_count_entries(pLd, searchRes))
    {
        dwError = ERROR_INVALID_DATA;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    entry = ldap_first_entry(pLd, searchRes);
    if (!entry)
    {
        dwError = ERROR_INVALID_ENTRY;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    pszDn = ldap_get_dn(pLd, entry);
    if (IsNullOrEmptyString(pszDn))
    {
        dwError = ERROR_INVALID_DN;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirAllocateStringA(pszDn, &pszReplHostNameDN);
    BAIL_ON_VMDIR_ERROR(dwError);
    *ppszReplHostNameDN = pszReplHostNameDN;

cleanup:
    ldap_memfree(pszDn);
    ldap_msgfree(searchRes);
    VMDIR_SAFE_FREE_MEMORY(pszSearchFilter);
    return dwError;

error:
    *ppszReplHostNameDN = NULL;
    VMDIR_SAFE_FREE_MEMORY(pszReplHostNameDN);
    goto cleanup;
}

/*
 * Used in VmDirCreateCMSubtree
 */
static
DWORD
VmDirAddCMSiteNode(
    LDAP* pLd,
    PCSTR pszDomain,
    PCSTR pszSiteGUID)
{
    DWORD       dwError = 0;
    PSTR        pszDN = NULL;

    PCSTR       valsCn[] = {pszSiteGUID, NULL};
    PCSTR       valsClass[] = {CM_OBJECTCLASS_SITE, NULL};
    PCSTR       valsDisname[] = {CM_DISPLAYNAME_SITE, NULL};

    LDAPMod     mod[3]={
                            {LDAP_MOD_ADD, ATTR_CN, {(PSTR*)valsCn}},
                            {LDAP_MOD_ADD, ATTR_OBJECT_CLASS, {(PSTR*)valsClass}},
                            {LDAP_MOD_ADD, ATTR_DISPLAY_NAME, {(PSTR*)valsDisname}}
                       };
    LDAPMod*    attrs[] = {&mod[0], &mod[1], &mod[2], NULL};
    PSTR        pszDomainDN = NULL;

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(&pszDN,
                                            "cn=%s,cn=%s,cn=%s,%s",
                                            pszSiteGUID,
                                            CM_SITE,
                                            CM_COMPONENTMANAGER,
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Adding %s.", pszDN);

    dwError = ldap_add_ext_s(
                             pLd,
                             pszDN,
                             attrs,
                             NULL,
                             NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirAddCMSiteNode failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * Used in VmDirCreateCMSubtree
 */
static
DWORD
VmDirAddLduNode(
    LDAP* pLd,
    PCSTR pszDomain,
    PCSTR pszSiteGUID,
    PCSTR pszLduGUID)
{
    DWORD       dwError = 0;
    PSTR        pszDN = NULL;
    PCSTR       valsCn[] = {pszLduGUID, NULL};
    PCSTR       valsClass[] = {CM_OBJECTCLASS_LDU, NULL};
    PCSTR       valsDisname[] = {NULL, NULL};
    PCSTR       valsSite[] = {pszSiteGUID, NULL};
    PSTR        pszDisName = NULL;

    LDAPMod     mod[]={
                            {LDAP_MOD_ADD, ATTR_CN, {(PSTR*)valsCn}},
                            {LDAP_MOD_ADD, ATTR_OBJECT_CLASS, {(PSTR*)valsClass}},
                            {LDAP_MOD_ADD, ATTR_SITE_ID, {(PSTR*)valsSite}},
                            {LDAP_MOD_ADD, ATTR_DISPLAY_NAME, {(PSTR*)valsDisname}}
                       };
    LDAPMod*    attrs[] = {&mod[0], &mod[1], &mod[2], &mod[3], NULL};
    PSTR        pszDomainDN = NULL;

    dwError = VmDirAllocateStringAVsnprintf(&pszDisName,
                                            "Deployment %s",
                                            pszLduGUID);
    valsDisname[0] = pszDisName;

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(&pszDN,
                                            "cn=%s,cn=%s,cn=%s,%s",
                                            pszLduGUID,
                                            CM_LDUS,
                                            CM_COMPONENTMANAGER,
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Adding %s.", pszDN);

    dwError = ldap_add_ext_s(
                             pLd,
                             pszDN,
                             attrs,
                             NULL,
                             NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pszDisName);
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirAddLduNode failed. Error(%u)", dwError);
    goto cleanup;
}

static
BOOL
VmDirIsSolutionUser(
    PCSTR   pszUserDN
    )
{
    DWORD   dwError = 0;
    PSTR    pszDomainName = NULL;
    PSTR    pszDomainDN = NULL;
    CHAR    pszLduGuid[VMDIR_GUID_STR_LEN] = {0};
    PSTR    pszLduDN = NULL;

    dwError = VmDirGetDomainName(
                            "localhost",
                            &pszDomainName
                            );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirGetLocalLduGuid(pszLduGuid);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(&pszLduDN,
                                            "cn=%s,cn=%s,cn=%s,%s",
                                            pszLduGuid,
                                            CM_LDUS,
                                            CM_COMPONENTMANAGER,
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (VmDirCaselessStrStrA(pszUserDN, pszLduDN))
    {
        dwError = 0;
    }
    else
    {
        dwError = 1;
    }

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDomainName);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pszLduDN);
    return dwError == 0;

error:
    goto cleanup;
}

static
DWORD
VmDirLdapGetAttributeValues(
    LDAP* pLd,
    PCSTR pszDN,
    PCSTR pszAttribute,
    LDAPControl** ppSrvCtrls,
    BerValue*** pppBerValues
    )
{
    DWORD           dwError = 0;
    PCSTR           ppszAttrs[] = {pszAttribute, NULL};
    LDAPMessage*    pSearchRes = NULL;
    LDAPMessage*    pEntry = NULL;
    BerValue**      ppBerValues = NULL;

    dwError = ldap_search_ext_s(
                            pLd,
                            pszDN,
                            LDAP_SCOPE_BASE,
                            NULL,       /* filter */
                            (PSTR*)ppszAttrs,
                            FALSE,      /* attrsonly */
                            ppSrvCtrls, /* serverctrls */
                            NULL,       /* clientctrls */
                            NULL,       /* timeout */
                            0,         /* sizelimit */
                            &pSearchRes);
    BAIL_ON_VMDIR_ERROR(dwError);

    pEntry = ldap_first_entry(pLd, pSearchRes);
    if (!pEntry)
    {
        dwError = LDAP_NO_SUCH_ATTRIBUTE;
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    ppBerValues = ldap_get_values_len(pLd, pEntry, pszAttribute);
    if (!ppBerValues)
    {
        dwError = LDAP_NO_SUCH_ATTRIBUTE;
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    *pppBerValues = ppBerValues;
cleanup:
    if (pSearchRes)
    {
        ldap_msgfree(pSearchRes);
    }
    return dwError;
error:
    *pppBerValues = NULL;
    if(ppBerValues)
    {
        ldap_value_free_len(ppBerValues);
    }

    VMDIR_LOG_ERROR( VMDIR_LOG_MASK_ALL, "VmDirLdapGetAttributeValues failed. Error(%u)", dwError);
    goto cleanup;
}

static
DWORD
VmDirLdapWriteAttributeValues(
    LDAP* pLd,
    PCSTR pszDN,
    PCSTR pszAttribute,
    PCSTR pszValue
    )
{
    DWORD       dwError = 0;
    LDAPMod     mod = {0};
    LDAPMod*    mods[2] = {&mod, NULL};
    PSTR        vals[2] = {(PSTR)pszValue, NULL};

    mod.mod_op = LDAP_MOD_ADD;
    mod.mod_type = (PSTR)pszAttribute;
    mod.mod_vals.modv_strvals = vals;

    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Add %s - %s:%s", pszDN, pszAttribute, pszValue);

    dwError = ldap_modify_ext_s(
                            pLd,
                            pszDN,
                            mods,
                            NULL,
                            NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    return dwError;
error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapWriteAttributeValues failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * Helper function
 * Get first attribute from pEntry to pszAttrTarget which is CHAR array on stack
 * Used in VmDirMessagetoReplicationInfo
 */
static
DWORD
VmDirGetReplicationAttr(
    LDAP* pLd,
    LDAPMessage* pEntry,
    PCSTR pszAttribute,
    PSTR pszAttrTarget,
    DWORD dwAttrLen)
{
    DWORD       dwError = 0;
    BerValue**  ppBerValues = NULL;
    PSTR        pszDN = NULL;

    pszDN = ldap_get_dn(pLd, pEntry);
    assert(pszDN);

    dwError = VmDirLdapGetAttributeValues(
                                        pLd,
                                        pszDN,
                                        pszAttribute,
                                        NULL,
                                        &ppBerValues);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirStringNCpyA(pszAttrTarget, dwAttrLen, ppBerValues[0]->bv_val, ppBerValues[0]->bv_len);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    if (pszDN)
    {
        ldap_memfree(pszDN);
    }

    if (ppBerValues)
    {
        ldap_value_free_len(ppBerValues);
    }
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirGetReplicationAttr failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * Helper function
 * Get replication information from pMessage to pInfo
 * Used in VmDirGetReplicationInfo
 */
static
DWORD
VmDirMessagetoReplicationInfo(
    LDAP* pLd,
    LDAPMessage* pEntry,
    PREPLICATION_INFO pInfo)
{
    DWORD       dwError = 0;
    BerElement* ber = NULL;
    PSTR        attr = NULL;

    for (attr = ldap_first_attribute(pLd, pEntry, &ber);
         attr;
         attr = ldap_next_attribute(pLd, pEntry, ber))
    {
        VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Attribute:%s", attr);

        if (VmDirStringCompareA(attr, ATTR_LABELED_URI, FALSE) == 0)
        {
            dwError = VmDirGetReplicationAttr(pLd, pEntry, attr, pInfo->pszURI, VMDIR_MAX_LDAP_URI_LEN);
            BAIL_ON_VMDIR_ERROR(dwError);
        }

        ldap_memfree(attr);
        attr = NULL;
    }

cleanup:
    if (ber)
    {
        ber_free(ber,0);
    }
    return dwError;

error:
    if (attr)
    {
        ldap_memfree(attr);
    }

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirMessagetoReplicationInfo failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * assume pszDN : cn=xxx,.....
 * return *ppszCN=xxx
 */
DWORD
VmDirDnLastRDNToCn(
    PCSTR   pszDN,
    PSTR*   ppszCN
    )
{
    DWORD   dwError = 0;
    PSTR    pszCN = NULL;
    int     i = 0;

    while (pszDN[i] !=',' && pszDN[i] != '\0')
    {
        i++;
    }

    dwError = VmDirAllocateMemory(i+1, (VOID*)&pszCN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirStringNCpyA(pszCN, i, pszDN+3, i-3);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszCN = pszCN;
cleanup:
    return dwError;
error:
    VMDIR_SAFE_FREE_MEMORY(pszCN);
    printf("VmDirDnToCn failed. Error[%d]\n", dwError);
    goto cleanup;
}

static
DWORD
VmDirMergeGroup(
    LDAP*   pSourceLd,
    LDAP*   pTargetLd,
    PCSTR   pszSourceDomainDN,
    PCSTR   pszTargetDomainDN,
    PCSTR   pszSourceGroupDN
    )
{
    DWORD           dwError = 0;
    PSTR            pszTargetGroupDN = NULL;
    BerValue**      ppBerValues = NULL;
    PSTR            pszGroupCN = NULL;
    int             i = 0;

    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Merging group: %s", VDIR_SAFE_STRING(pszSourceGroupDN));

    dwError = VmDirGetTargetDN(pszSourceDomainDN, pszTargetDomainDN, pszSourceGroupDN, &pszTargetGroupDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (!VmDirIfDNExist(pTargetLd, pszTargetGroupDN))
    {
        dwError = VmDirDnLastRDNToCn(pszTargetGroupDN, &pszGroupCN);
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = VmDirAddVmIdentityGroup(
                                        pTargetLd,
                                        pszGroupCN,
                                        pszTargetGroupDN
                                        );
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirLdapGetAttributeValues(
                                    pSourceLd,
                                    pszSourceGroupDN,
                                    ATTR_MEMBER,
                                    NULL,
                                    &ppBerValues);
    //if no members, we just return success
    if (dwError == LDAP_NO_SUCH_ATTRIBUTE)
    {
        dwError = ERROR_SUCCESS;
        goto cleanup;
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    for (i=0; ppBerValues[i]; i++)
    {
        PCSTR pszUserDN = ppBerValues[i]->bv_val;
        //we only migrate solution users
        if (VmDirIsSolutionUser(pszUserDN))
        {
            VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Merging member: %s", pszUserDN);
            dwError = VmDirLdapWriteAttributeValues(
                                            pTargetLd,
                                            pszTargetGroupDN,
                                            ATTR_MEMBER,
                                            pszUserDN);
            //ignore error for conflict
            if (dwError == LDAP_TYPE_OR_VALUE_EXISTS)
            {
                dwError = ERROR_SUCCESS;
            }
            BAIL_ON_VMDIR_ERROR(dwError);
        }
    }

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszGroupCN);
    VMDIR_SAFE_FREE_MEMORY(pszTargetGroupDN);
    if(ppBerValues)
    {
        ldap_value_free_len(ppBerValues);
    }

    return dwError;
error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirMergeGroup failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * ####################### Shared Functions in libvmdirclient ########################
 */
/*
 * Used in VmDirCopyLDAPSubTree
 */
DWORD
VmDirGetTargetDN(
    PCSTR pszSourceBase,
    PCSTR pszTargetBase,
    PCSTR pszSourceDN,
    PSTR* ppszTargetDN
    )
{
    DWORD   dwError = 0;
    PSTR    pszTargetDN = NULL;
    SIZE_T  sizeSource = 0;
    SIZE_T  sizeTarget = 0;
    SIZE_T  sizeSourceDN = 0;
    SIZE_T  sizeTargetDN = 0;

    sizeSource = VmDirStringLenA(pszSourceBase);
    sizeTarget = VmDirStringLenA(pszTargetBase);
    sizeSourceDN = VmDirStringLenA(pszSourceDN);
    sizeTargetDN = sizeSourceDN - sizeSource + sizeTarget +1;

    dwError = VmDirAllocateMemory(sizeTargetDN, (PVOID*)&pszTargetDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirStringNCpyA(pszTargetDN, sizeTargetDN, pszSourceDN, sizeSourceDN - sizeSource);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirStringCatA(pszTargetDN, sizeTargetDN, pszTargetBase);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszTargetDN = pszTargetDN;
cleanup:
    return dwError;

error:
    *ppszTargetDN = NULL;
    VMDIR_SAFE_FREE_MEMORY(pszTargetDN);
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirGetTargetDN failed. Error(%u)", dwError);
    goto cleanup;
}

BOOLEAN
VmDirIfDNExist(
    LDAP* pLd,
    PCSTR pszDN)
{
    DWORD           dwError = 0;
    LDAPMessage*    pSearchRes = NULL;

    dwError = ldap_search_ext_s(
                            pLd,
                            pszDN,         /* base */
                            LDAP_SCOPE_BASE,
                            NULL,       /* filter */
                            NULL,       /* attrs */
                            FALSE,      /* attrsonly */
                            NULL,       /* serverctrls */
                            NULL,       /* clientctrls */
                            NULL,       /* timeout */
                            0,          /* sizelimit */
                            &pSearchRes);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    if (pSearchRes)
    {
        ldap_msgfree(pSearchRes);
    }
    return dwError == 0;
error:
    goto cleanup;
}

/*
 * Get the host portion of LDAP URI ldap(s)://host:port
 */
DWORD
VmDirLdapURI2Host(
    PCSTR   pszURI,
    PSTR*   ppszHost
    )
{
    DWORD   dwError = 0;
    PCSTR   pszSlashSeperator = NULL;
    PCSTR   pszPortSeperator = NULL;
    PCSTR   pszStartHostName = NULL;
    PSTR    pszLocalHostName = NULL;
    size_t  iSize = 0;

    if ( !pszURI || !ppszHost )
    {
        dwError = VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    // skip "ldap(s)//:" part in pszURI
    if ((pszSlashSeperator = strchr( pszURI, '/')) != NULL)
    {
        pszStartHostName = pszSlashSeperator + 2; // skip
    }
    else
    {
        pszStartHostName =  pszURI;
    }

    pszPortSeperator = strrchr(pszStartHostName, ':');

    iSize = pszPortSeperator ? (pszPortSeperator - pszStartHostName) :
                                VmDirStringLenA( pszStartHostName);

    dwError = VmDirAllocateMemory(  iSize + 1, (PVOID*) &pszLocalHostName);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirCopyMemory( pszLocalHostName,
                               iSize,
                               (const PVOID)pszStartHostName,
                               iSize);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszHost = pszLocalHostName;

cleanup:

    return dwError;

error:

    VMDIR_SAFE_FREE_MEMORY( pszLocalHostName );

    goto cleanup;
}

DWORD
VmDirConnectLDAPServerByURI(
    LDAP**      ppLd,
    PCSTR       pszLdapURI,
    PCSTR       pszDomain,
    PCSTR       pszUserName,
    PCSTR       pszPassword
    )
{
    DWORD       dwError = 0;
    PSTR        pszHost = NULL;
    LDAP*       pLocalLd = NULL;

    if (ppLd == NULL || pszLdapURI == NULL || IsNullOrEmptyString(pszDomain) ||
            IsNullOrEmptyString(pszUserName) || pszPassword==NULL) //allows empty password
    {
        dwError = VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirLdapURI2Host( pszLdapURI, &pszHost );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirConnectLDAPServer(   &pLocalLd,
                                        pszHost,
                                        pszDomain,
                                        pszUserName,
                                        pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppLd = pLocalLd;

cleanup:

    VMDIR_SAFE_FREE_MEMORY( pszHost );

    return dwError;
error:

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirConnectLDAPServerByURI failed. Error(%u)(%s)",
                                        dwError, VDIR_SAFE_STRING( pszLdapURI));

    if ( pLocalLd )
    {
        ldap_unbind_ext_s(pLocalLd, NULL, NULL);
    }

    goto cleanup;
}

/*
 * connect to LDAP via srp or krb mechanism
 */
DWORD
VmDirConnectLDAPServer(
    LDAP**      ppLd,
    PCSTR       pszHostName,
    PCSTR       pszDomain,
    PCSTR       pszUserName,
    PCSTR       pszPassword
    )
{
    DWORD   dwError = 0;
    char    bufUPN[VMDIR_MAX_UPN_LEN] = {0};
    PSTR    pszLdapURI = NULL;
    PSTR    pszUserDN = NULL;
    PSTR    pszDomainDN = NULL;

    dwError = VmDirStringPrintFA( bufUPN, sizeof(bufUPN)-1,  "%s@%s", pszUserName, pszDomain);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSafeLDAPBind( ppLd,
                                 pszHostName,
                                 bufUPN,
                                 pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pszLdapURI);
    VMDIR_SAFE_FREE_MEMORY(pszUserDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirConnectLDAPServer failed. (%u)(%s)", dwError, bufUPN);
    goto cleanup;
}

VOID
VmDirLdapUnbind(
    LDAP** ppLd
    )
{
    if (ppLd && *ppLd)
    {
        ldap_unbind_ext_s(*ppLd, NULL, NULL);
        *ppLd = NULL;
    }
}

DWORD
VmDirGetSiteGuidInternal(
    LDAP* pLd,
    PCSTR pszDomain,
    PSTR* ppszGUID
    )
{
    DWORD dwError = 0;
    PSTR  pszFilter = "(objectclass=*)";
    PSTR  pszSiteName = NULL;
    PSTR  pszSiteDN = NULL;
    PSTR  pszAttrObjectGUID = "objectGUID";
    PSTR  ppszAttrs[] = { pszAttrObjectGUID, NULL };
    LDAPMessage *pResult = NULL;
    LDAPMessage *pEntry = NULL;
    struct berval** ppValues = NULL;
    PSTR  pszGUID = NULL;

    dwError = VmDirGetSiteName(pLd, &pszSiteName);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirGetSiteDN(pszDomain, pszSiteName, &pszSiteDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = ldap_search_ext_s(
                    pLd,
                    pszSiteDN,
                    LDAP_SCOPE_BASE,
                    pszFilter,
                    ppszAttrs,
                    FALSE, /* get values      */
                    NULL,  /* server controls */
                    NULL,  /* client controls */
                    NULL,  /* timeout         */
                    0,     /* size limit      */
                    &pResult);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (ldap_count_entries(pLd, pResult) != 1) /* expect only one site object */
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    pEntry = ldap_first_entry(pLd, pResult);
    if (!pEntry)
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    ppValues = ldap_get_values_len(pLd, pEntry, pszAttrObjectGUID);

    if (!ppValues || (ldap_count_values_len(ppValues) != 1))
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirAllocateStringPrintf(
                        &pszGUID,
                        "%s",
                        ppValues[0]->bv_val);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszGUID = pszGUID;

cleanup:

    if (ppValues)
    {
        ldap_value_free_len(ppValues);
    }

    if (pResult)
    {
        ldap_msgfree(pResult);
    }

    VMDIR_SAFE_FREE_MEMORY(pszSiteName);
    VMDIR_SAFE_FREE_MEMORY(pszSiteDN);

    return dwError;

error:

    if (ppszGUID)
    {
        *ppszGUID = NULL;
    }

    VMDIR_SAFE_FREE_MEMORY(pszGUID);

    goto cleanup;
}

DWORD
VmDirGetSiteName(
    LDAP* pLd,
    PSTR* ppszSiteName
    )
{
    DWORD dwError = 0;
    PSTR  pszSiteName = NULL;
    LDAPMessage *pResult = NULL;
    PSTR  pszAttrSiteName = "msDS-SiteName";
    PSTR  ppszAttrs[] = { pszAttrSiteName, NULL };
    PSTR  pszFilter = "(objectclass=*)";
    struct berval** ppValues = NULL;

    if (!pLd || !ppszSiteName)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = ldap_search_ext_s(
                pLd,
                NULL,
                LDAP_SCOPE_BASE,
                pszFilter,
                &ppszAttrs[0],
                FALSE, /* get values also */
                NULL,  /* server controls */
                NULL,  /* client controls */
                NULL,  /* timeout         */
                0,     /* size limit      */
                &pResult);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (ldap_count_entries(pLd, pResult) > 0)
    {
        LDAPMessage *pEntry = ldap_first_entry(pLd, pResult);

        if (pEntry)
        {
            ppValues = ldap_get_values_len(pLd, pEntry, pszAttrSiteName);

            if (ppValues && ldap_count_values_len(ppValues) > 0)
            {
                dwError = VmDirAllocateStringPrintf(
                                &pszSiteName,
                                "%s",
                                ppValues[0]->bv_val);
                BAIL_ON_VMDIR_ERROR(dwError);
            }
        }
    }

    if (IsNullOrEmptyString(pszSiteName))
    {
        dwError = ERROR_NO_SITENAME;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    *ppszSiteName = pszSiteName;

cleanup:

    if (ppValues)
    {
        ldap_value_free_len(ppValues);
    }

    if (pResult)
    {
        ldap_msgfree(pResult);
    }

    return dwError;

error:

    if (ppszSiteName)
    {
        *ppszSiteName = NULL;
    }

    VMDIR_SAFE_FREE_MEMORY(pszSiteName);

    goto cleanup;
}

DWORD
VmDirGetSiteDN(
    PCSTR pszDomain,
    PCSTR pszSiteName,
    PSTR* ppszSiteDN
    )
{
    DWORD dwError = 0;
    PSTR  pszDomainDN = NULL;
    PSTR  pszSiteDN = NULL;

    if (IsNullOrEmptyString(pszDomain) ||
        IsNullOrEmptyString(pszSiteName) ||
        !ppszSiteDN)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringPrintf(
                    &pszSiteDN,
                    "CN=%s,CN=Sites,CN=Configuration,%s",
                    pszSiteName,
                    pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszSiteDN = pszSiteDN;

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    return dwError;

error:

    if (ppszSiteDN)
    {
        *ppszSiteDN = NULL;
    }

    VMDIR_SAFE_FREE_MEMORY(pszSiteDN);

    goto cleanup;
}

/*
 * Used in VmDirCreateCMSubtree
 */
DWORD
VmDirAddVmIdentityContainer(
    LDAP* pLd,
    PCSTR pszCN,
    PCSTR pszDN
    )
{
    DWORD       dwError = 0;
    PCSTR       valsCn[] = {pszCN, NULL};
    PCSTR       valsClass[] = {OC_TOP, OC_CONTAINER, NULL};
    LDAPMod     mod[2]={
                            {LDAP_MOD_ADD, ATTR_CN, {(PSTR*)valsCn}},
                            {LDAP_MOD_ADD, ATTR_OBJECT_CLASS, {(PSTR*)valsClass}}
                       };
    LDAPMod*    attrs[] = {&mod[0], &mod[1], NULL};

    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Adding %s.", pszDN);

    dwError = ldap_add_ext_s(
                             pLd,
                             pszDN,
                             attrs,
                             NULL,
                             NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirAddVmIdentityContainer failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * Copy subtree from source to target.
 * Used in merge.c and split.c
 */
DWORD
VmDirCopyLDAPSubTree(
    LDAP    *pLdSource,
    LDAP    *pLdTarget,
    PCSTR   pszSourceBase,
    PCSTR   pszTargetBase,
    PCSTR   pszSourceDomainDN,
    PCSTR   pszTargetDomainDN
    )
{
    DWORD           dwError = 0;
    LDAPMessage*    entry = NULL;
    PDEQUE          pDeque = NULL;
    PSTR            pszDN = NULL;
    PSTR            pszSourceDN = NULL;
    PSTR            pszTargetDN = NULL;
    LDAPMessage*    searchRes = NULL;
    PCSTR           ppszAttrs[] = {"*","-",NULL};
    PBYTE           pOldMasterKey = NULL;
    PBYTE           pNewMasterKey = NULL;
    DWORD           dwOldMasterKeyLen = 0;
    DWORD           dwNewMasterKeyLen = 0;

    if (pLdSource == NULL || pLdTarget == NULL || pszSourceBase == NULL || pszTargetBase == NULL)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }
/*
    dwError = VmDirLdapGetMasterKey(
                                pLdSource,
                                pszSourceDomainDN,
                                &pOldMasterKey,
                                &dwOldMasterKeyLen);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapGetMasterKey(
                                pLdTarget,
                                pszTargetDomainDN,
                                &pNewMasterKey,
                                &dwNewMasterKeyLen);
    BAIL_ON_VMDIR_ERROR(dwError);
*/
    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Copying data from '%s' to '%s'", pszSourceBase, pszTargetBase);
    dwError = dequeCreate(&pDeque);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = dequePush(pDeque, (PVOID)pszSourceBase);
    BAIL_ON_VMDIR_ERROR(dwError);

    while (!dequeIsEmpty(pDeque))
    {
        dwError = dequePopLeft(pDeque, (PVOID*)&pszDN);
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = (LDAP_SUCCESS !=
                   ldap_search_ext_s(
                                     pLdSource,
                                     pszDN,
                                     LDAP_SCOPE_ONELEVEL,
                                     NULL,  /* filter */
                                     (char**)ppszAttrs,
                                     FALSE,
                                     NULL,  /* serverctrls */
                                     NULL,  /* clientctrls */
                                     NULL,  /* timeout */
                                     0,
                                     &searchRes));
        BAIL_ON_VMDIR_ERROR(dwError);

        for (entry = ldap_first_entry(pLdSource, searchRes);
             entry != NULL;
             entry = ldap_next_entry(pLdSource, entry))
        {
            pszSourceDN = ldap_get_dn(pLdSource, entry);
            dwError = VmDirGetTargetDN(pszSourceBase, pszTargetBase, pszSourceDN, &pszTargetDN);
            BAIL_ON_VMDIR_ERROR(dwError);

            dwError = VmDirLdapCopyEntry(pLdSource, pLdTarget, entry, pszTargetDN, pOldMasterKey, dwOldMasterKeyLen, pNewMasterKey, dwNewMasterKeyLen);
            //if entry exists already, we will ignore the error and continue
            if (dwError == ERROR_SUCCESS || dwError == LDAP_ALREADY_EXISTS)
            {
                dwError = ERROR_SUCCESS;
                dwError = dequePush(pDeque, (PVOID)ldap_get_dn(pLdSource, entry));
                BAIL_ON_VMDIR_ERROR(dwError);
            }
            else
            {
                BAIL_ON_VMDIR_ERROR(dwError);
            }
        }

        if (pszSourceDN)
        {
            ldap_memfree(pszSourceDN);
            pszSourceDN = NULL;
        }

        if (pszTargetDN)
        {
            VmDirFreeMemory(pszTargetDN);
            pszTargetDN = NULL;
        }

        if (searchRes)
        {
            ldap_msgfree(searchRes);
            searchRes = NULL;
        }

        if (pszDN && pszDN != pszSourceBase)
        {
            ldap_memfree(pszDN);
            pszDN = NULL;
        }
    }

cleanup:
    if (pDeque)
    {
        dequeFree(pDeque);
    }
    return dwError;

error:
    VMDIR_SAFE_FREE_MEMORY(pOldMasterKey);
    VMDIR_SAFE_FREE_MEMORY(pNewMasterKey);

    if (pszSourceDN)
    {
        ldap_memfree(pszSourceDN);
    }

    VMDIR_SAFE_FREE_MEMORY(pszTargetDN);

    if (searchRes)
    {
        ldap_msgfree(searchRes);
    }

    if (pszDN && pszDN != pszSourceBase)
    {
        ldap_memfree(pszDN);
    }

    while (!dequeIsEmpty(pDeque))
    {
        PSTR pszTmp = NULL;
        dequePopLeft(pDeque, (PVOID*)&pszTmp);
        ldap_memfree(pszTmp);
    }

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirCopyLDAPSubTree failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * Get all replication info of a server (pszHost) from pLd
 * 1. server entry DN: cn=SERVER_NAME,cn=server,cn=SITE_NAME,cn=sites,cn=configuration,DOMAIN_DN
 * 2. find all replication agreement entries under cn=SITE_NAME,cn=sites,cn=configuration,DOMAIN_DN
 * 3. filter out SERVER_NAME == pszHost
 */
DWORD
VmDirGetReplicationInfo(
    LDAP* pLd,
    PCSTR pszHost,
    PCSTR pszDomain,
    PREPLICATION_INFO* ppReplicationInfo,
    DWORD* pdwInfoCount
    )
{
    DWORD               dwError = 0;
    PSTR                pszSearchBaseDN = NULL;
    PCSTR               ppszAttrs[] = {ATTR_LABELED_URI, NULL};
    LDAPMessage*        pSearchRes = NULL;
    LDAPMessage*        pEntry = NULL;
    PREPLICATION_INFO   pReplicationInfo = NULL;
    int                 i = 0;
    DWORD               dwInfoCount = 0;
    PSTR                pszDomainDN = NULL;
    PSTR                pszEntryDN = NULL;
    PSTR                pszHostMatch = NULL;

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(&pszSearchBaseDN,
                                            "cn=Sites,cn=Configuration,%s",
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = ldap_search_ext_s(
                             pLd,
                             pszSearchBaseDN,
                             LDAP_SCOPE_SUBTREE,
                             "objectclass=vmwReplicationAgreement", /* filter */
                             (PSTR*)ppszAttrs,  /* attrs[]*/
                             FALSE,
                             NULL,              /* serverctrls */
                             NULL,              /* clientctrls */
                             NULL,              /* timeout */
                             0,
                             &pSearchRes);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwInfoCount = ldap_count_entries(pLd, pSearchRes);
    if (dwInfoCount > 0)
    {
        dwError = VmDirAllocateMemory(dwInfoCount*sizeof(REPLICATION_INFO), (PVOID*)&pReplicationInfo);
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = VmDirAllocateStringAVsnprintf(
                        &pszHostMatch,
                        "cn=Replication Agreements,cn=%s,cn=Servers,",
                        pszHost);
        BAIL_ON_VMDIR_ERROR(dwError);

        for (i=0, pEntry = ldap_first_entry(pLd, pSearchRes);
             pEntry;
             pEntry = ldap_next_entry(pLd, pEntry))
        {
            pszEntryDN = ldap_get_dn(pLd, pEntry);
            if (VmDirStringCaseStrA( pszEntryDN, pszHostMatch ))
            {
                dwError = VmDirMessagetoReplicationInfo(pLd, pEntry, &pReplicationInfo[i]);
                BAIL_ON_VMDIR_ERROR(dwError);
                i++;
            }

            ldap_memfree(pszEntryDN);
            pszEntryDN=NULL;
        }
    }

    *ppReplicationInfo = pReplicationInfo;
    *pdwInfoCount = i;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszSearchBaseDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pszHostMatch);

    ldap_memfree(pszEntryDN);
    if (pSearchRes)
    {
        ldap_msgfree(pSearchRes);
    }
    return dwError;

error:
    *ppReplicationInfo = NULL;
    *pdwInfoCount = 0;
    VmDirFreeMemory(pReplicationInfo);
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirGetReplicationInfo failed. Error(%u)", dwError);
    goto cleanup;
}

/*
 * Find all RA DN that has pszHost as it replication parnter
 * examine labeledURI and find pszHost exists.
 * NOTE, this works only if labeledURI is created using consistent naming scheme.
 * i.e. pszHost = DCAccount (in registry)
 *
 */
DWORD
VmDirGetAllRAToHost(
    LDAP*   pLD,
    PCSTR   pszHost,
    PSTR**  pppRADNArray,
    DWORD*  pdwSize
    )
{
    DWORD               dwError = 0;
    PSTR                pszSiteDN = NULL;
    LDAPMessage*        pSearchRes = NULL;
    LDAPMessage*        pEntry = NULL;
    DWORD               dwCnt = 0;
    DWORD               dwSearchSize = 0;
    PSTR                pszDomain=NULL;
    PSTR*               ppLocalArray=NULL;
    PSTR                pszDomainDN = NULL;
    PSTR                pszEntryDN = NULL;

    if ( !pLD || !pszHost || !pppRADNArray || !pdwSize )
    {
        dwError = VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirGetDomainName( "localhost", &pszDomain );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(&pszSiteDN,
                                            "cn=Sites,cn=Configuration,%s",
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = ldap_search_ext_s(
                             pLD,
                             pszSiteDN,
                             LDAP_SCOPE_SUBTREE,
                             "objectclass=vmwReplicationAgreement", /* filter */
                             NULL,              /* attrs[]*/
                             FALSE,
                             NULL,              /* serverctrls */
                             NULL,              /* clientctrls */
                             NULL,              /* timeout */
                             0,
                             &pSearchRes);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwSearchSize = ldap_count_entries(pLD, pSearchRes);
    if (dwSearchSize > 0)
    {
        dwError = VmDirAllocateMemory(dwSearchSize*sizeof(PSTR), (PVOID*)&ppLocalArray);
        BAIL_ON_VMDIR_ERROR(dwError);

        for (dwCnt=0, pEntry = ldap_first_entry(pLD, pSearchRes);
             pEntry;
             pEntry = ldap_next_entry(pLD, pEntry))
        {
            PSTR    pszHostMatch = NULL;
            PSTR    pszRAMatch = NULL;

            pszEntryDN = ldap_get_dn(pLD, pEntry);
            // DN: labeledURI=ldap(s)://PARTNER_HOST:port,cn=Replication Agreements,cn=LOCAL_DCACCOUNT,.....
            pszHostMatch = VmDirStringCaseStrA( pszEntryDN, pszHost );
            if ( pszHostMatch )
            {
                pszRAMatch = VmDirStringCaseStrA( pszEntryDN, ",cn=Replication Agreements,cn=" );
                if ( pszRAMatch && (pszHostMatch < pszRAMatch) ) // pszHost match DN PARTNER_HOST section
                {
                    dwError = VmDirAllocateStringA( pszEntryDN, &(ppLocalArray[dwCnt]) );
                    BAIL_ON_VMDIR_ERROR(dwError);
                    dwCnt++;
                }
            }

            ldap_memfree(pszEntryDN);
            pszEntryDN=NULL;
        }
    }

    *pppRADNArray = ppLocalArray;
    *pdwSize      = dwCnt;
    ppLocalArray  = NULL;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszSiteDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomain);

    ldap_memfree(pszEntryDN);
    if (pSearchRes)
    {
        ldap_msgfree(pSearchRes);
    }

    return dwError;

error:
    for (dwCnt=0; dwCnt < dwSearchSize; dwCnt++)
    {
        VMDIR_SAFE_FREE_MEMORY( ppLocalArray[dwCnt] );
    }
    VMDIR_SAFE_FREE_MEMORY(ppLocalArray);
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirGetAllRAToHost failed. Error(%u)", dwError);

    goto cleanup;
}

static
DWORD
VmDirStoreLduGuidtoDC(
    LDAP* pLd,
    PCSTR pszDomainDn,
    PCSTR pszLduGuid)
{
    DWORD   dwError = 0;
    PSTR    pszDN = NULL;
    char    pszHostName[VMDIR_MAX_HOSTNAME_LEN];

    // vdcpromo sets this key.
    dwError = VmDirGetRegKeyValue( VMDIR_CONFIG_PARAMETER_KEY_PATH,
                                   VMDIR_REG_KEY_DC_ACCOUNT,
                                   pszHostName,
                                   sizeof(pszHostName)-1);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(&pszDN,
                                            "cn=%s,ou=%s,%s",
                                            pszHostName,
                                            VMDIR_DOMAIN_CONTROLLERS_RDN_VAL,
                                            pszDomainDn);
    BAIL_ON_VMDIR_ERROR(dwError);

    //ATTR_LDU_GUID
    dwError = VmDirLdapModReplaceAttribute(
                                        pLd,
                                        pszDN,
                                        ATTR_LDU_GUID,
                                        pszLduGuid);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDN);
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirStoreLduGuidtoDC failed. Error(%u)", dwError);
    goto cleanup;
}
/*
 * Create Component Manager Subtree in directory, including ComponentManager, CMSites, Ldus, Site-Guid and Ldu-Guid
 * Store Ldu-GUID into DC object
 * Used in setupldu.c and split.c
 */
DWORD
VmDirCreateCMSubtree(
    void* pvLd,
    PCSTR pszDomain,
    PCSTR pszSiteGuid,
    PCSTR pszLduGuid)
{
    DWORD   dwError = 0;
    PSTR    pszDN = NULL;
    PSTR    pszDomainDN = NULL;
    LDAP* pLd = (LDAP *) pvLd;

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);

    //create "ComponentManager"
    dwError = VmDirAllocateStringAVsnprintf(&pszDN,
                                            "cn=%s,%s",
                                            CM_COMPONENTMANAGER,
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddVmIdentityContainer(pLd, CM_COMPONENTMANAGER, pszDN);
    //if CM node already exists, ignore error. It happens for join scenario. Replication happens before running vdcsetupldu
    if (dwError == LDAP_ALREADY_EXISTS)
    {
        dwError = ERROR_SUCCESS;
    }
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_SAFE_FREE_MEMORY(pszDN);

    //create "Ldus"
    dwError = VmDirAllocateStringAVsnprintf(&pszDN,
                                            "cn=%s,cn=%s,%s",
                                            CM_LDUS,
                                            CM_COMPONENTMANAGER,
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddVmIdentityContainer(pLd, CM_LDUS, pszDN);
    //if Ldus node already exists, ignore
    if (dwError == LDAP_ALREADY_EXISTS)
    {
        dwError = ERROR_SUCCESS;
    }
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_SAFE_FREE_MEMORY(pszDN);

    //create "CMSites"
    dwError = VmDirAllocateStringAVsnprintf(&pszDN,
                                            "cn=%s,cn=%s,%s",
                                            CM_SITE,
                                            CM_COMPONENTMANAGER,
                                            pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddVmIdentityContainer(pLd, CM_SITE, pszDN);
    //if CMSites container node already exists, ignore
    if (dwError == LDAP_ALREADY_EXISTS)
    {
        dwError = ERROR_SUCCESS;
    }
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_SAFE_FREE_MEMORY(pszDN);

    //add Site
    dwError = VmDirAddCMSiteNode(
                            pLd,
                            pszDomain,
                            pszSiteGuid);
    //if CMSites node already exists (created by first infrastructure vm), ignore
    if (dwError == LDAP_ALREADY_EXISTS)
    {
        dwError = ERROR_SUCCESS;
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    //Add Ldu
    dwError = VmDirAddLduNode(
                            pLd,
                            pszDomain,
                            pszSiteGuid,
                            pszLduGuid);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirStoreLduGuidtoDC(
                                pLd,
                                pszDomainDN,
                                pszLduGuid);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirCreateCMSubtree failed. Error(%u)", dwError);
    goto cleanup;
}


DWORD
VmDirGetServerName(
    PCSTR pszHostName,
    PSTR* ppszServerName)
{
    DWORD       dwError = 0;
    PSTR        pszHostURI = NULL;
    LDAP*       pLd = NULL;
    PSTR        pszServerName = NULL;
    BerValue**  ppBerValues = NULL;

    if (IsNullOrEmptyString(pszHostName) || ppszServerName == NULL)
    {
        dwError =  LDAP_INVALID_SYNTAX;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if ( VmDirIsIPV6AddrFormat( pszHostName ) )
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszHostURI, "%s://[%s]:%d",
                                                 VMDIR_LDAP_PROTOCOL,
                                                 pszHostName,
                                                 DEFAULT_LDAP_PORT_NUM);
    }
    else
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszHostURI, "%s://%s:%d",
                                                 VMDIR_LDAP_PROTOCOL,
                                                 pszHostName,
                                                 DEFAULT_LDAP_PORT_NUM);
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAnonymousLDAPBind( &pLd, pszHostURI );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapGetAttributeValues(
                                pLd,
                                "",
                                ATTR_SERVER_NAME,
                                NULL,
                                &ppBerValues);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirDnLastRDNToCn(ppBerValues[0]->bv_val, &pszServerName);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszServerName = pszServerName;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszHostURI);

    if (pLd)
    {
        ldap_unbind_ext_s(pLd, NULL, NULL);
    }

    if(ppBerValues)
    {
        ldap_value_free_len(ppBerValues);
    }

    return dwError;
error:
    *ppszServerName = NULL;
    VMDIR_SAFE_FREE_MEMORY(pszServerName);

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirGetServerName failed with error (%u)", dwError);
    goto cleanup;
}

/*
 * Setup one way replication agreement
 */
DWORD
VmDirLdapSetupRemoteHostRA(
    PCSTR pszDomainName,
    PCSTR pszHostName,
    PCSTR pszUsername,
    PCSTR pszPassword,
    PCSTR pszReplHostName
    )
{
    DWORD       dwError = 0;
    PSTR        pszReplURI = NULL;
    PSTR        pszReplHostNameDN = NULL;
    PSTR        pszReplAgrDN = NULL;

    LDAP*       pLd = NULL;
    PSTR        pszDomainDN = NULL;

    if ( VmDirIsIPV6AddrFormat( pszReplHostName ) )
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszReplURI, "%s://[%s]", VMDIR_LDAP_PROTOCOL, pszReplHostName);
    }
    else
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszReplURI, "%s://%s", VMDIR_LDAP_PROTOCOL, pszReplHostName);
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirConnectLDAPServer(
                            &pLd,
                            pszHostName,
                            pszDomainName,
                            pszUsername,
                            pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapCreateReplHostNameDN(&pszReplHostNameDN, pLd, pszHostName);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(
                                    &pszReplAgrDN,
                                    "labeledURI=%s,cn=%s,%s",
                                    pszReplURI,      // uri points back to local server
                                    VMDIR_REPL_AGRS_CONTAINER_NAME,
                                    pszReplHostNameDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (pszReplAgrDN > 0)
    {

        char* modv_agr[] = {OC_REPLICATION_AGREEMENT, OC_TOP, NULL};
        char* modv_uri[] = {pszReplURI, NULL};
        char* modv_usn[] = {VMDIR_DEFAULT_REPL_LAST_USN_PROCESSED, NULL};
        LDAPMod replAgreement = {0};
        LDAPMod replURI = {0};
        LDAPMod replUSN = {0};

        LDAPMod* pReplAgrObjAttrs[] =
        {
                &replAgreement,
                &replURI,
                &replUSN,
                NULL
        };

        replAgreement.mod_op = LDAP_MOD_ADD;
        replAgreement.mod_type = ATTR_OBJECT_CLASS;
        replAgreement.mod_values = modv_agr;

        replURI.mod_op = LDAP_MOD_ADD;
        replURI.mod_type = ATTR_LABELED_URI;
        replURI.mod_values = modv_uri;

        replUSN.mod_op = LDAP_MOD_ADD;
        replUSN.mod_type = ATTR_LAST_LOCAL_USN_PROCESSED;
        replUSN.mod_values = modv_usn;

        // and the ldap_add_ext_s is a synchronous call
        dwError = ldap_add_ext_s(pLd, pszReplAgrDN, &pReplAgrObjAttrs[0], NULL, NULL);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszReplURI);
    VMDIR_SAFE_FREE_MEMORY(pszReplHostNameDN);
    VMDIR_SAFE_FREE_MEMORY(pszReplAgrDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    VmDirLdapUnbind(&pLd);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapSetupRemoteHostRA failed with error (%u)", dwError);
    goto cleanup;
}

/*
 * Remove one way replication agreement
 */
DWORD
VmDirLdapRemoveRemoteHostRA(
    PCSTR pszDomainName,
    PCSTR pszHostName,
    PCSTR pszUsername,
    PCSTR pszPassword,
    PCSTR pszReplHostName
    )
{
    DWORD       dwError = 0;
    PSTR        pszReplURI = NULL;
    PSTR        pszReplHostNameDN = NULL;
    PSTR        pszReplAgrDN = NULL;

    LDAP*       pLd = NULL;
    PSTR        pszDomainDN = NULL;

    if ( VmDirIsIPV6AddrFormat( pszReplHostName ) )
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszReplURI, "%s://[%s]", VMDIR_LDAP_PROTOCOL, pszReplHostName);
    }
    else
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszReplURI, "%s://%s", VMDIR_LDAP_PROTOCOL, pszReplHostName);
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirConnectLDAPServer(
                            &pLd,
                            pszHostName,
                            pszDomainName,
                            pszUsername,
                            pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapCreateReplHostNameDN(&pszReplHostNameDN, pLd, pszHostName);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(
                                    &pszReplAgrDN,
                                    "labeledURI=%s,cn=%s,%s",
                                    pszReplURI,
                                    VMDIR_REPL_AGRS_CONTAINER_NAME,
                                    pszReplHostNameDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    // the ldap_delete_ext_s is a synchronous call
    dwError = ldap_delete_ext_s(
                    pLd,
                    pszReplAgrDN,
                    NULL,
                    NULL
                    );
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszReplURI);
    VMDIR_SAFE_FREE_MEMORY(pszReplHostNameDN);
    VMDIR_SAFE_FREE_MEMORY(pszReplAgrDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    VmDirLdapUnbind(&pLd);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapRemoveRemoteHostRA failed with error (%u)", dwError);
    goto cleanup;
}

#ifdef _WIN32

static  _TCHAR  RSA_SERVER_CERT[MAX_PATH];
static  _TCHAR  RSA_SERVER_KEY[MAX_PATH];

#endif
/*
static
DWORD
_VmDirSetCertificate(BerValue *    pbvCertificate)
{
    DWORD       dwError = 0;
    FILE *      certFp = NULL;
    size_t      rLen = 0;
    PSTR        pszLocalErrMsg = NULL;

#ifdef _WIN32
    dwError = VmDirOpensslSetServerCertPath(RSA_SERVER_CERT);
    BAIL_ON_VMDIR_ERROR(dwError);
#endif

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirSetCertificate() reading certificate file (%s)", RSA_SERVER_CERT) ;

    if ((certFp = fopen((const char *)RSA_SERVER_CERT, "rb")) == NULL)
    {
        dwError = VMDIR_ERROR_NO_SUCH_FILE_OR_DIRECTORY;
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrMsg, "fopen(%s, rb) failed", RSA_SERVER_CERT );
    }

    if ( fseek( certFp, 0L, SEEK_END ) != 0 )
    {
        dwError = VMDIR_ERROR_IO;
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrMsg, "fseek( certFp, 0L, SEEK_END ) failed" );
    }

    pbvCertificate->bv_len = ftell( certFp );

    if ( fseek( certFp, 0L, SEEK_SET ) != 0 )
    {
        dwError = VMDIR_ERROR_IO;
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrMsg, "fseek( certFp, 0L, SEEK_SET ) failed" );
    }

    dwError = VmDirAllocateMemory(pbvCertificate->bv_len, (PVOID*)&(pbvCertificate->bv_val));
    BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrMsg, "VmDirAllocateMemory() failed (%d)", pbvCertificate->bv_len );

    rLen = fread( pbvCertificate->bv_val, 1, pbvCertificate->bv_len, certFp );

    if (rLen != pbvCertificate->bv_len)
    {
        dwError = VMDIR_ERROR_IO;
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrMsg, "rLen = %d, bv_len = %d", rLen, pbvCertificate->bv_len );
    }

cleanup:
    if (certFp)
    {
        fclose(certFp);
    }
    VMDIR_SAFE_FREE_MEMORY( pszLocalErrMsg );
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirSetCertificate() failed with error message (%s), error code (%u)",
                    VDIR_SAFE_STRING(pszLocalErrMsg), dwError);

    VMDIR_SAFE_FREE_MEMORY(pbvCertificate->bv_val);

    goto cleanup;
}
*/

/*
 * Setup this Domain Controller account on the partner host
 */
DWORD
VmDirLdapSetupDCAccountOnPartner(
    PCSTR pszDomainName,
    PCSTR pszHostName,              // Partner host name
    PCSTR pszUsername,
    PCSTR pszPassword,
    PCSTR pszDCHostName             // Self host name
    )
{
    DWORD       dwError = 0;
    PSTR        pszDCDN = NULL;
    LDAP*       pLd = NULL;
    PSTR        pszDomainDN = NULL;
    PBYTE       pByteDCAccountPasswd = NULL;
    DWORD       dwDCAccountPasswdSize = 0;
    PSTR        pszUPN = NULL;
    PSTR        pszUpperCaseDomainName = NULL;
    PSTR        pszLowerCaseDCHostName = NULL;
    BOOLEAN     bIsLocalLdapServer = TRUE;
    PCSTR       pszServerName = NULL;
    DWORD       dwRetries = 0;
    BOOLEAN     bAcctExists=FALSE;
    PSTR        pszSRPUPN = NULL;
    PSTR        pszMachineGUID = NULL;

    char* modv_oc[] = {OC_PERSON, OC_ORGANIZATIONAL_PERSON, OC_USER, OC_COMPUTER, OC_TOP, NULL};
    char* modv_cn[] = {(PSTR)pszDCHostName, NULL};
    char* modv_sam[] = {(PSTR)pszDCHostName, NULL};
    char* modv_upn[] = {(PSTR)NULL, NULL};
    char* modv_machine[] = {(PSTR)NULL, NULL};

    BerValue    bvPasswd = {0};
    BerValue*   pbvPasswd[2] = { NULL, NULL};

    LDAPMod modObjectClass = {0};
    LDAPMod modCn = {0};
    LDAPMod modPwd = {0};
    LDAPMod modSamAccountName = {0};
    LDAPMod modUserPrincipalName = {0};
    LDAPMod modMachineGUID = {0};

    LDAPMod* pDCMods[] =
    {
            &modObjectClass,
            &modCn,
            &modPwd,
            &modSamAccountName,
            &modUserPrincipalName,
            &modMachineGUID,
            NULL
    };

    if (IsNullOrEmptyString(pszDomainName) ||
        IsNullOrEmptyString(pszHostName) ||
        IsNullOrEmptyString(pszUsername) ||
        IsNullOrEmptyString(pszPassword) ||
        IsNullOrEmptyString(pszDCHostName))
    {
        dwError =  VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    modObjectClass.mod_op = LDAP_MOD_ADD;
    modObjectClass.mod_type = ATTR_OBJECT_CLASS;
    modObjectClass.mod_values = modv_oc;

    modCn.mod_op = LDAP_MOD_ADD;
    modCn.mod_type = ATTR_CN;
    modCn.mod_values = modv_cn;

    modPwd.mod_op = LDAP_MOD_ADD | LDAP_MOD_BVALUES;
    modPwd.mod_type = ATTR_USER_PASSWORD;
    modPwd.mod_bvalues = pbvPasswd;
    pbvPasswd[0] = &bvPasswd;

    modSamAccountName.mod_op = LDAP_MOD_ADD;
    modSamAccountName.mod_type = ATTR_SAM_ACCOUNT_NAME;
    modSamAccountName.mod_values = modv_sam;

    modMachineGUID.mod_op = LDAP_MOD_ADD;
    modMachineGUID.mod_type = ATTR_MACHINE_GUID;
    modMachineGUID.mod_values = modv_machine;

    dwError = VmDirAllocateMemory( VMDIR_GUID_STR_LEN,
                                   (PVOID*)&pszMachineGUID );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirGenerateGUID(pszMachineGUID);
    BAIL_ON_VMDIR_ERROR(dwError);

    modv_machine[0] = pszMachineGUID;

    dwError = VmDirAllocASCIILowerToUpper( pszDomainName, &pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocASCIIUpperToLower( pszDCHostName, &pszLowerCaseDCHostName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszUPN, "%s@%s", pszLowerCaseDCHostName, pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszSRPUPN, "%s@%s", pszUsername, pszDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    modv_upn[0] = pszUPN;

    modUserPrincipalName.mod_op = LDAP_MOD_ADD;
    modUserPrincipalName.mod_type = ATTR_KRB_UPN;
    modUserPrincipalName.mod_values = modv_upn;

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszDCDN, "%s=%s,%s=%s,%s", ATTR_CN, pszLowerCaseDCHostName,
                                             ATTR_OU, VMDIR_DOMAIN_CONTROLLERS_RDN_VAL, pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if ( VmDirStringCompareA( pszHostName, pszDCHostName, FALSE ) != 0 )
    {   // BUGBUG, does not consider simple vs fqdn name scenario.
        // It handles case from VmDirSetupHostInstance and VmDirJoin
        bIsLocalLdapServer = FALSE;
    }

    // use localhost if ldap server is local, so NIMBUS will work as is.
    pszServerName = (bIsLocalLdapServer ? "localhost" : pszHostName);

    dwError = VmDirConnectLDAPServer( &pLd,
                                      pszServerName,
                                      pszDomainName,
                                      pszUsername,
                                      pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    for (dwRetries=0; dwRetries < VMDIR_MAX_PASSWORD_RETRIES; dwRetries++)
    {
        dwError = VmDirGeneratePassword( pszHostName, // partner host in DC join scenario
                                         pszSRPUPN,
                                         pszPassword,
                                         &pByteDCAccountPasswd,
                                         &dwDCAccountPasswdSize );
        BAIL_ON_VMDIR_ERROR(dwError);

        bvPasswd.bv_val = pByteDCAccountPasswd;
        bvPasswd.bv_len = dwDCAccountPasswdSize;

        // and the ldap_add_ext_s is a synchronous call
        dwError = ldap_add_ext_s(pLd, pszDCDN, &pDCMods[0], NULL, NULL);
        if (dwError == LDAP_SUCCESS || dwError != LDAP_CONSTRAINT_VIOLATION)
        {
            break;
        }

        VMDIR_SAFE_FREE_MEMORY(pByteDCAccountPasswd);
        dwDCAccountPasswdSize = 0;
    }

    if ( dwError == LDAP_ALREADY_EXISTS )
    {
        dwError = 0;
        bAcctExists = TRUE;

        // reset DCAccount password. NOTE pByteDCAccountPasswd is null terminted.
        dwError = VmDirLdapModReplaceAttribute( pLd, pszDCDN, ATTR_USER_PASSWORD, pByteDCAccountPasswd );
        BAIL_ON_VMDIR_ERROR(dwError);
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "DC account (%s) created (recycle %s)", pszDCDN, bAcctExists ? "T":"F");

    // add DCAccount into DCAdmins group
    dwError = _VmDirLdapSetupAccountMembership( pLd, pszDomainDN, VMDIR_DC_GROUP_NAME, pszDCDN );
    BAIL_ON_VMDIR_ERROR(dwError);

    // Store DC account password in registry.
    dwError = VmDirConfigSetDCAccountInfo(
                    pszDCHostName,
                    pszDCDN,
                    pByteDCAccountPasswd,
                    dwDCAccountPasswdSize,
                    pszMachineGUID );
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDCDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pByteDCAccountPasswd);
    VMDIR_SAFE_FREE_MEMORY(pszUPN);
    VMDIR_SAFE_FREE_MEMORY(pszUpperCaseDomainName);
    VMDIR_SAFE_FREE_MEMORY(pszLowerCaseDCHostName);
    VMDIR_SAFE_FREE_MEMORY(pszSRPUPN);
    VMDIR_SAFE_FREE_MEMORY(pszMachineGUID);

    VmDirLdapUnbind(&pLd);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapSetupDCAccountOnPartner (%s) failed with error (%u)",
                    VDIR_SAFE_STRING(pszDCDN), dwError);
    goto cleanup;
}

/*
 * Setup this Computer account on the remote host
 */
DWORD
VmDirLdapSetupComputerAccount(
    PCSTR pszDomainName,
    PCSTR pszHostName,              // Remote host name
    PCSTR pszUsername,
    PCSTR pszPassword,
    PCSTR pszComputerHostName       // Self host name
    )
{
    DWORD       dwError = 0;
    PSTR        pszComputerDN = NULL;
    LDAP*       pLd = NULL;
    PSTR        pszDomainDN = NULL;
    PBYTE       pByteAccountPasswd = NULL;
    DWORD       dwAccountPasswdSize = 0;
    PSTR        pszUPN = NULL;
    PSTR        pszUpperCaseDomainName = NULL;
    PSTR        pszLowerCaseComputerHostName = NULL;
    BOOLEAN     bIsLocalLdapServer = TRUE;
    PCSTR       pszServerName = NULL;
    PSTR        pszSiteGUID = NULL;
    PSTR        pszMachineGUID = NULL;
    DWORD       dwRetries = 0;
    BOOLEAN     bAcctExists = FALSE;
    PSTR        pszSRPUPN = NULL;

    char* modv_oc[] = {OC_PERSON, OC_ORGANIZATIONAL_PERSON, OC_USER, OC_COMPUTER, OC_TOP, NULL};
    char* modv_cn[] = {(PSTR)pszComputerHostName, NULL};
    char* modv_sam[] = {(PSTR)pszComputerHostName, NULL};
    char* modv_site[] = {(PSTR)NULL, NULL};
    char* modv_machine[] = {(PSTR)NULL, NULL};
    char* modv_upn[] = {(PSTR)NULL, NULL};

    BerValue    bvPasswd = {0};
    BerValue*   pbvPasswd[2] = { NULL, NULL};

    LDAPMod modObjectClass = {0};
    LDAPMod modCn = {0};
    LDAPMod modPwd = {0};
    LDAPMod modSamAccountName = {0};
    LDAPMod modUserPrincipalName = {0};
    LDAPMod modSiteGUID = {0};
    LDAPMod modMachineGUID = {0};

    LDAPMod* pDCMods[] =
    {
            &modObjectClass,
            &modCn,
            &modPwd,
            &modSamAccountName,
            &modUserPrincipalName,
            &modSiteGUID,
            &modMachineGUID,
            NULL
    };

    if (IsNullOrEmptyString(pszDomainName) ||
        IsNullOrEmptyString(pszHostName) ||
        IsNullOrEmptyString(pszUsername) ||
        IsNullOrEmptyString(pszPassword) ||
        IsNullOrEmptyString(pszComputerHostName))
    {
        dwError =  VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    modObjectClass.mod_op = LDAP_MOD_ADD;
    modObjectClass.mod_type = ATTR_OBJECT_CLASS;
    modObjectClass.mod_values = modv_oc;

    modCn.mod_op = LDAP_MOD_ADD;
    modCn.mod_type = ATTR_CN;
    modCn.mod_values = modv_cn;

    modPwd.mod_op = LDAP_MOD_ADD | LDAP_MOD_BVALUES;
    modPwd.mod_type = ATTR_USER_PASSWORD;
    modPwd.mod_bvalues = pbvPasswd;
    pbvPasswd[0] = &bvPasswd;

    modSamAccountName.mod_op = LDAP_MOD_ADD;
    modSamAccountName.mod_type = ATTR_SAM_ACCOUNT_NAME;
    modSamAccountName.mod_values = modv_sam;

    modSiteGUID.mod_op = LDAP_MOD_ADD;
    modSiteGUID.mod_type = ATTR_SITE_GUID;
    modSiteGUID.mod_values = modv_site;

    modMachineGUID.mod_op = LDAP_MOD_ADD;
    modMachineGUID.mod_type = ATTR_MACHINE_GUID;
    modMachineGUID.mod_values = modv_machine;

    dwError = VmDirAllocateStringAVsnprintf( &pszSRPUPN, "%s@%s", pszUsername, pszDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocASCIILowerToUpper( pszDomainName, &pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocASCIIUpperToLower(
                    pszComputerHostName,
                    &pszLowerCaseComputerHostName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(
                    &pszUPN,
                    "%s@%s",
                    pszLowerCaseComputerHostName,
                    pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    modv_upn[0] = pszUPN;

    modUserPrincipalName.mod_op = LDAP_MOD_ADD;
    modUserPrincipalName.mod_type = ATTR_KRB_UPN;
    modUserPrincipalName.mod_values = modv_upn;

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(
                    &pszComputerDN,
                    "%s=%s,%s=%s,%s",
                    ATTR_CN,
                    pszLowerCaseComputerHostName,
                    ATTR_OU,
                    VMDIR_COMPUTERS_RDN_VAL,
                    pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if ( VmDirStringCompareA( pszHostName, pszComputerHostName, FALSE ) != 0 )
    {   // BUGBUG, does not consider simple vs fqdn name scenario.
        // It handles case from VmDirSetupHostInstance and VmDirJoin
        bIsLocalLdapServer = FALSE;
    }

    // use localhost if ldap server is local, so NIMBUS will work as is.
    pszServerName = (bIsLocalLdapServer ? "localhost" : pszHostName);

    dwError = VmDirConnectLDAPServer( &pLd,
                                      pszServerName,
                                      pszDomainName,
                                      pszUsername,
                                      pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirGetSiteGuidInternal(pLd, pszDomainName, &pszSiteGUID);
    BAIL_ON_VMDIR_ERROR(dwError);

    modv_site[0] = pszSiteGUID;

    dwError = VmDirAllocateMemory( VMDIR_GUID_STR_LEN,
                                   (PVOID*)&pszMachineGUID );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirGenerateGUID(pszMachineGUID);
    BAIL_ON_VMDIR_ERROR(dwError);

    modv_machine[0] = pszMachineGUID;

    for (dwRetries=0; dwRetries < VMDIR_MAX_PASSWORD_RETRIES; dwRetries++)
    {
        dwError = VmDirGeneratePassword( pszServerName,
                                         pszSRPUPN,
                                         pszPassword,
                                         &pByteAccountPasswd,
                                         &dwAccountPasswdSize );
        BAIL_ON_VMDIR_ERROR(dwError);

        bvPasswd.bv_val = pByteAccountPasswd;
        bvPasswd.bv_len = dwAccountPasswdSize;

        // and the ldap_add_ext_s is a synchronous call
        dwError = ldap_add_ext_s(pLd, pszComputerDN, &pDCMods[0], NULL, NULL);
        if (dwError == LDAP_SUCCESS || dwError != LDAP_CONSTRAINT_VIOLATION)
        {
            break;
        }

        VMDIR_SAFE_FREE_MEMORY(pByteAccountPasswd);
        dwAccountPasswdSize = 0;
    }

    if ( dwError == LDAP_ALREADY_EXISTS )
    {
        dwError = 0;
        bAcctExists = TRUE;

        // reset ComputerAccount password. NOTE pByteDCAccountPasswd is null terminted.
        dwError = VmDirLdapModReplaceAttribute( pLd, pszComputerDN, ATTR_USER_PASSWORD, pByteAccountPasswd );
        BAIL_ON_VMDIR_ERROR(dwError);
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "Computer account (%s) created (recycle %s)", pszComputerDN, bAcctExists ? "T":"F");

    // add Computer Account into DCClients group
    dwError = _VmDirLdapSetupAccountMembership( pLd, pszDomainDN, VMDIR_DCCLIENT_GROUP_NAME, pszComputerDN );
    BAIL_ON_VMDIR_ERROR(dwError);

    // Store Computer account info in registry.
    dwError = VmDirConfigSetDCAccountInfo(
                    pszComputerHostName,
                    pszComputerDN,
                    pByteAccountPasswd,
                    dwAccountPasswdSize,
                    pszMachineGUID);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszComputerDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pByteAccountPasswd);
    VMDIR_SAFE_FREE_MEMORY(pszUPN);
    VMDIR_SAFE_FREE_MEMORY(pszUpperCaseDomainName);
    VMDIR_SAFE_FREE_MEMORY(pszLowerCaseComputerHostName);
    VMDIR_SAFE_FREE_MEMORY(pszSiteGUID);
    VMDIR_SAFE_FREE_MEMORY(pszSRPUPN);
    VMDIR_SAFE_FREE_MEMORY(pszMachineGUID);

    VmDirLdapUnbind(&pLd);

    return dwError;

error:

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapSetupComputerAccount (%s) failed with error (%u)",
                    VDIR_SAFE_STRING(pszComputerDN), dwError);
    goto cleanup;
}

/*
 * Remove this Computer account on the remote host
 */
DWORD
VmDirLdapRemoveComputerAccount(
    PCSTR pszDomainName,
    PCSTR pszHostName,              // Remote host name
    PCSTR pszUsername,
    PCSTR pszPassword,
    PCSTR pszComputerHostName       // Self host name
    )
{
    DWORD       dwError = 0;
    PSTR        pszComputerDN = NULL;
    LDAP*       pLd = NULL;
    PSTR        pszDomainDN = NULL;
    PSTR        pszUPN = NULL;
    PSTR        pszUpperCaseDomainName = NULL;
    PSTR        pszLowerCaseComputerHostName = NULL;
    BOOLEAN     bIsLocalLdapServer = TRUE;
    PCSTR       pszServerName = NULL;

    if (IsNullOrEmptyString(pszDomainName) ||
        IsNullOrEmptyString(pszHostName) ||
        IsNullOrEmptyString(pszUsername) ||
        IsNullOrEmptyString(pszPassword) ||
        IsNullOrEmptyString(pszComputerHostName))
    {
        dwError =  VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }


    dwError = VmDirAllocASCIILowerToUpper( pszDomainName, &pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocASCIIUpperToLower(
                    pszComputerHostName,
                    &pszLowerCaseComputerHostName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(
                    &pszUPN,
                    "%s@%s",
                    pszLowerCaseComputerHostName,
                    pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf(
                    &pszComputerDN,
                    "%s=%s,%s=%s,%s",
                    ATTR_CN,
                    pszLowerCaseComputerHostName,
                    ATTR_OU,
                    VMDIR_COMPUTERS_RDN_VAL,
                    pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if ( VmDirStringCompareA( pszHostName, pszComputerHostName, FALSE ) != 0 )
    {   // BUGBUG, does not consider simple vs fqdn name scenario.
        // It handles case from VmDirSetupHostInstance and VmDirJoin
        bIsLocalLdapServer = FALSE;
    }

    // use localhost if ldap server is local, so NIMBUS will work as is.
    pszServerName = (bIsLocalLdapServer ? "localhost" : pszHostName);

    dwError = VmDirConnectLDAPServer( &pLd,
                                      pszServerName,
                                      pszDomainName,
                                      pszUsername,
                                      pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = ldap_delete_ext_s(pLd, pszComputerDN, NULL, NULL);
    BAIL_ON_VMDIR_ERROR(dwError);


cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszComputerDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pszUPN);
    VMDIR_SAFE_FREE_MEMORY(pszUpperCaseDomainName);
    VMDIR_SAFE_FREE_MEMORY(pszLowerCaseComputerHostName);

    VmDirLdapUnbind(&pLd);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapRemoveComputerAccount (%s) failed with error (%u)",
                    VDIR_SAFE_STRING(pszComputerDN), dwError);
    goto cleanup;
}


/*
 * Setup this Domain Controller's ldap service account on the partner host
 */
DWORD
VmDirLdapSetupServiceAccount(
    PCSTR pszDomainName,
    PCSTR pszHostName,          // Partner host name
    PCSTR pszUsername,
    PCSTR pszPassword,
    PCSTR pszServiceName,
    PCSTR pszDCHostName         // Self host name
    )
{
    DWORD       dwError = 0;
    PSTR        pszMSADN = NULL;
    LDAP*       pLd = NULL;
    PSTR        pszDomainDN = NULL;
    PBYTE       pByteMSAPasswd = NULL;
    DWORD       dwMSAPasswdSize = 0;
    PSTR        pszUPN = NULL;
    PSTR        pszName = NULL;
    PSTR        pszUpperCaseDomainName = NULL;
    PSTR        pszLowerCaseDCHostName = NULL;
    BOOLEAN     bIsLocalLdapServer = TRUE;
    PCSTR       pszServerName = NULL;
    BOOLEAN     bAcctExists = FALSE;
    PSTR        pszSRPUPN = NULL;

    char* modv_oc[] = {OC_MANAGED_SERVICE_ACCOUNT, OC_ORGANIZATIONAL_PERSON, OC_USER, OC_COMPUTER,
                       OC_TOP, NULL};
    char* modv_cn[] = {(PSTR)NULL, NULL};
    char* modv_sam[] = {(PSTR)NULL, NULL};
    char* modv_upn[] = {(PSTR)NULL, NULL};
    BerValue    bvPasswd = {0};
    BerValue*   pbvPasswd[2] = { NULL, NULL};

    LDAPMod modObjectClass = {0};
    LDAPMod modCn = {0};
    LDAPMod modPwd = {0};
    LDAPMod modSamAccountName = {0};
    LDAPMod modUserPrincipalName = {0};

    LDAPMod* pDCMods[] =
    {
            &modObjectClass,
            &modCn,
            &modPwd,
            &modSamAccountName,
            &modUserPrincipalName,
            NULL
    };

    modObjectClass.mod_op = LDAP_MOD_ADD;
    modObjectClass.mod_type = ATTR_OBJECT_CLASS;
    modObjectClass.mod_values = modv_oc;

    dwError = VmDirAllocateStringAVsnprintf( &pszName, "%s/%s", pszServiceName, pszDCHostName );
    BAIL_ON_VMDIR_ERROR(dwError);

    modv_cn[0] = modv_sam[0] = pszName;

    modCn.mod_op = LDAP_MOD_ADD;
    modCn.mod_type = ATTR_CN;
    modCn.mod_values = modv_cn;

    modPwd.mod_op = LDAP_MOD_ADD | LDAP_MOD_BVALUES;
    modPwd.mod_type = ATTR_USER_PASSWORD;
    modPwd.mod_bvalues = &(pbvPasswd[0]);
    pbvPasswd[0] = &bvPasswd;

    modSamAccountName.mod_op = LDAP_MOD_ADD;
    modSamAccountName.mod_type = ATTR_SAM_ACCOUNT_NAME;
    modSamAccountName.mod_values = modv_sam;

    dwError = VmDirAllocateStringAVsnprintf( &pszSRPUPN, "%s@%s", pszUsername, pszDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocASCIILowerToUpper( pszDomainName, &pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocASCIIUpperToLower( pszDCHostName, &pszLowerCaseDCHostName);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszUPN, "%s/%s@%s", pszServiceName, pszLowerCaseDCHostName, pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    modv_upn[0] = pszUPN;

    modUserPrincipalName.mod_op = LDAP_MOD_ADD;
    modUserPrincipalName.mod_type = ATTR_KRB_UPN;
    modUserPrincipalName.mod_values = modv_upn;

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszMSADN, "%s=%s,%s=%s,%s", ATTR_CN, pszUPN,
                                             ATTR_CN, VMDIR_MSAS_RDN_VAL, pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if ( VmDirStringCompareA( pszHostName, pszDCHostName, FALSE ) != 0 )
    {   // BUGBUG, does not consider simple vs fqdn name scenario.
        // It handles case from VmDirSetupHostInstance and VmDirJoin
        bIsLocalLdapServer = FALSE;
    }

    pszServerName = (bIsLocalLdapServer ? "localhost" : pszHostName);

    dwError = VmDirConnectLDAPServer( &pLd,
                                      pszServerName,
                                      pszDomainName,
                                      pszUsername,
                                      pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    while( TRUE )
    {
        dwError = VmDirGeneratePassword( pszHostName, // partner host in DC join scenario
                                         pszSRPUPN,
                                         pszPassword,
                                         (PBYTE*)(&pByteMSAPasswd),
                                         &dwMSAPasswdSize );
        BAIL_ON_VMDIR_ERROR(dwError);

        bvPasswd.bv_val = pByteMSAPasswd;
        bvPasswd.bv_len = dwMSAPasswdSize;

        // and the ldap_add_ext_s is a synchronous call
        dwError = ldap_add_ext_s(pLd, pszMSADN, &pDCMods[0], NULL, NULL);
        if ( dwError == LDAP_SUCCESS )
        {
            break;
        }
        else if ( dwError == LDAP_ALREADY_EXISTS )
        {
            bAcctExists = TRUE;
            dwError = 0;
            break;
        }
        else if (dwError == LDAP_CONSTRAINT_VIOLATION)
        {
            VMDIR_SAFE_FREE_MEMORY(pByteMSAPasswd);
            dwMSAPasswdSize = 0;
            continue;
        }
        else
        {
            BAIL_ON_VMDIR_ERROR(dwError);
        }
    }

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "Service account (%s) created (recycle %s)", pszMSADN, bAcctExists ? "T":"F");

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszMSADN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pByteMSAPasswd);
    VMDIR_SAFE_FREE_MEMORY(pszUPN);
    VMDIR_SAFE_FREE_MEMORY(pszName);
    VMDIR_SAFE_FREE_MEMORY(pszUpperCaseDomainName);
    VMDIR_SAFE_FREE_MEMORY(pszLowerCaseDCHostName);
    VMDIR_SAFE_FREE_MEMORY(pszSRPUPN);

    VmDirLdapUnbind(&pLd);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapSetupServiceAccountOnPartner (%s) failed with error (%u)",
                    VDIR_SAFE_STRING(pszMSADN), dwError);
    goto cleanup;
}

/*
 * Delete this Domain Controller account on the partner host
 */
DWORD
VmDirLdapDeleteDCAccountOnPartner(
    PCSTR   pszDomainName,
    PCSTR   pszHostName,              // Partner host name
    PCSTR   pszUsername,
    PCSTR   pszPassword,
    PCSTR   pszDCHostName,             // Self host name
    BOOLEAN bActuallyDelete
    )
{
    DWORD       dwError = 0;
    LDAP*       pLd = NULL;

    dwError = VmDirConnectLDAPServer( &pLd, pszHostName, pszDomainName, pszUsername, pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapDeleteDCAccount( pLd, pszDomainName, pszDCHostName, bActuallyDelete);
    BAIL_ON_VMDIR_ERROR(dwError);

    /* TBD: Optionally: Remove DC account password from registry.
    if (bActuallyDelete)
    {
        dwError = VmDirConfigSetDCAccountInfo( pszDCDN, pByteDCAccountPasswd, dwDCAccountPasswdSize );
        BAIL_ON_VMDIR_ERROR(dwError);
    }
    */

cleanup:

    VmDirLdapUnbind(&pLd);

    return dwError;

error:
    VMDIR_LOG_ERROR( VMDIR_LOG_MASK_ALL, "VmDirLdapDeleteDCAccountOnPartner failed with error (%u)",
                     dwError);
    goto cleanup;
}

/*
 * Delete a service account, that belongs to this Domain Controller, on the partner host
 */
DWORD
VmDirLdapDeleteServiceAccountOnPartner(
    PCSTR   pszDomainName,
    PCSTR   pszHostName,          // Partner host name
    PCSTR   pszUsername,
    PCSTR   pszPassword,
    PCSTR   pszServiceName,
    PCSTR   pszDCHostName,         // Self host name
    BOOLEAN bActuallyDelete
    )
{
    DWORD       dwError = 0;
    LDAP*       pLd = NULL;

    dwError = VmDirConnectLDAPServer( &pLd, pszHostName, pszDomainName, pszUsername, pszPassword);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapDeleteServiceAccount(pLd, pszDomainName, pszServiceName, pszDCHostName, bActuallyDelete);
    BAIL_ON_VMDIR_ERROR(dwError);


cleanup:

    VmDirLdapUnbind(&pLd);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapDeleteServiceAccountOnPartner failed with error (%u)",
                    dwError);
    goto cleanup;
}

DWORD
VmDirMergeGroups(
    LDAP*   pSourceLd,
    LDAP*   pTargetLd,
    PCSTR   pszSourceDomainDN,
    PCSTR   pszTargetDomainDN
    )
{
    DWORD           dwError = 0;
    PSTR            pszGroupDN = NULL;
    int i = 0;

    while (gGroupWhiteList[i])
    {
        dwError = VmDirAllocateStringAVsnprintf(&pszGroupDN, "%s,%s", gGroupWhiteList[i], pszSourceDomainDN);
        BAIL_ON_VMDIR_ERROR(dwError);

        if (VmDirIfDNExist(pSourceLd, pszGroupDN))
        {
            dwError = VmDirMergeGroup(
                                pSourceLd,
                                pTargetLd,
                                pszSourceDomainDN,
                                pszTargetDomainDN,
                                pszGroupDN
                );
            BAIL_ON_VMDIR_ERROR(dwError);
        }
        VMDIR_SAFE_FREE_MEMORY(pszGroupDN);
        i++;
    }

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszGroupDN);
    return dwError;
error:
    printf("VmDirMergeGroups failed. Error[%d]\n", dwError);
    goto cleanup;
}

DWORD
VmDirAddVmIdentityGroup(
    LDAP* pLd,
    PCSTR pszCN,
    PCSTR pszDN
    )
{
    DWORD       dwError = 0;
    PCSTR       valsCn[] = {pszCN, NULL};
    PCSTR       valsClass[] = {OC_GROUP, NULL};
    PCSTR       valsAccount[] = {pszCN, NULL};

    LDAPMod     mod[3]={
                            {LDAP_MOD_ADD, ATTR_CN, {(PSTR*)valsCn}},
                            {LDAP_MOD_ADD, ATTR_OBJECT_CLASS, {(PSTR*)valsClass}},
                            {LDAP_MOD_ADD, ATTR_SAM_ACCOUNT_NAME, {(PSTR*)valsAccount}}
                       };
    LDAPMod*    attrs[] = {&mod[0], &mod[1], &mod[2], NULL};

    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL, "Adding %s.", pszDN);

    dwError = ldap_add_ext_s(
                             pLd,
                             pszDN,
                             attrs,
                             NULL,
                             NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirAddVmIdentityGroup failed. Error(%u)", dwError);
    goto cleanup;
}

static
DWORD
_VmDirGetDSERootAttribute(
    PCSTR pszHostName,
    PCSTR pszAttrName,
    PSTR* ppszAttrValue)
{
    DWORD       dwError = 0;
    PSTR        pszLocalHostURI = NULL;
    LDAP*       pLd = NULL;
    PSTR        pszLocalAttrValue = NULL;
    BerValue**  ppBerValues = NULL;

    if (IsNullOrEmptyString(pszHostName) || IsNullOrEmptyString(pszAttrName) || ppszAttrValue == NULL)
    {
        dwError =  VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if ( VmDirIsIPV6AddrFormat( pszHostName ) )
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszLocalHostURI, "%s://[%s]:%d",
                                                 VMDIR_LDAP_PROTOCOL,
                                                 pszHostName,
                                                 DEFAULT_LDAP_PORT_NUM);
    }
    else
    {
        dwError = VmDirAllocateStringAVsnprintf( &pszLocalHostURI, "%s://%s:%d",
                                                 VMDIR_LDAP_PROTOCOL,
                                                 pszHostName,
                                                 DEFAULT_LDAP_PORT_NUM);
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAnonymousLDAPBind( &pLd, pszLocalHostURI );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapGetAttributeValues(
                                pLd,
                                "",
                                pszAttrName,
                                NULL,
                                &ppBerValues);
    BAIL_ON_VMDIR_ERROR(dwError);

    // BUGBUG BUGBUG, what if binary data?
    dwError = VmDirAllocateStringA(ppBerValues[0]->bv_val, &pszLocalAttrValue);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszAttrValue = pszLocalAttrValue;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszLocalHostURI);

    if (pLd)
    {
        ldap_unbind_ext_s(pLd, NULL, NULL);
    }

    if(ppBerValues)
    {
        ldap_value_free_len(ppBerValues);
    }

    return dwError;
error:
    *ppszAttrValue = NULL;
    VMDIR_SAFE_FREE_MEMORY(pszLocalAttrValue);

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirGetDSERootAttribute failed with error (%u)", dwError);
    goto cleanup;
}

/*
 * Public APIs
 */

DWORD
VmDirGetDomainDN(
    PCSTR pszHostName,
    PSTR* ppszDomainDN)
{
    return _VmDirGetDSERootAttribute(
                            pszHostName,
                            ATTR_ROOT_DOMAIN_NAMING_CONTEXT,
                            ppszDomainDN);
}

DWORD
VmDirGetServerDN(
    PCSTR pszHostName,
    PSTR* ppszServerDN
    )
{
    return _VmDirGetDSERootAttribute(
                            pszHostName,
                            ATTR_SERVER_NAME,
                            ppszServerDN);
}

DWORD
VmDirGetDomainName(
    PCSTR pszHostName,
    PSTR* ppszDomainName)
{
    DWORD dwError = 0;
    PSTR pszDomainDN = NULL;
    PSTR pszDomainName = NULL;

    dwError = VmDirGetDomainDN(pszHostName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirDomainDNToName(pszDomainDN, &pszDomainName);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszDomainName = pszDomainName;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    return dwError;
error:
    *ppszDomainName = NULL;
    VMDIR_SAFE_FREE_MEMORY(pszDomainName);

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirGetDomainName failed with error (%u)", dwError);
    goto cleanup;

}

DWORD
VmDirGetPartnerSiteName(
    PCSTR pszHostName,
    PSTR* ppszSiteName
    )
{
    return _VmDirGetDSERootAttribute(
                            pszHostName,
                            ATTR_SITE_NAME,
                            ppszSiteName);
}

DWORD
VmDirGetAdminDN(
    PCSTR pszHostName,
    PSTR* ppszAdminDN)
{
    return _VmDirGetDSERootAttribute(
                            pszHostName,
                            ATTR_DEFAULT_ADMIN_DN,
                            ppszAdminDN);
}

// SUNG, Bad naming if this is public api.  VmDirAdminNameFromHostName?
DWORD
VmDirGetAdminName(
    PCSTR pszHostName,
    PSTR* ppszAdminName)
{
    DWORD dwError=0;
    PSTR pszAdminDN = NULL;
    PSTR pszAdminName = NULL;

    dwError = VmDirGetAdminDN(pszHostName, &pszAdminDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirDnLastRDNToCn(pszAdminDN, &pszAdminName);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszAdminName = pszAdminName;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszAdminDN);
    return dwError;
error:
    *ppszAdminName = NULL;
    VMDIR_SAFE_FREE_MEMORY(pszAdminName);

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirGetAdminName failed with error (%u)", dwError);
    goto cleanup;
}

DWORD
VmDirLdapGetMasterKey(
    LDAP* pLd,
    PCSTR pszDomainDN,
    PBYTE* ppMasterKey,
    DWORD* pLen)
{
    DWORD           dwError = 0;
    BerValue**      ppBerValues = NULL;
    PBYTE           pMasterKey = NULL;
    LDAPControl     reqCtrl = {0};
    LDAPControl*    srvCtrls[2] = {0};

    if (!pLd || IsNullOrEmptyString(pszDomainDN) || !ppMasterKey || !pLen)
    {
        dwError =  VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    reqCtrl.ldctl_oid = VDIR_LDAP_CONTROL_SHOW_MASTER_KEY;
    reqCtrl.ldctl_iscritical = '1';

    srvCtrls[0] = &reqCtrl;
    srvCtrls[1] = NULL;

    dwError = VmDirLdapGetAttributeValues(
                                pLd,
                                pszDomainDN,
                                ATTR_KRB_MASTER_KEY,
                                srvCtrls,
                                &ppBerValues);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateAndCopyMemory(
                                ppBerValues[0]->bv_val,
                                ppBerValues[0]->bv_len,
                                (PVOID*)&pMasterKey);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppMasterKey = pMasterKey;
    *pLen = (DWORD)ppBerValues[0]->bv_len;

cleanup:
    if(ppBerValues)
    {
        ldap_value_free_len(ppBerValues);
    }

    return dwError;
error:
    *ppMasterKey = NULL;
    *pLen = 0;
    VMDIR_SAFE_FREE_MEMORY(pMasterKey);

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapGetMasterKey failed with error (%u)", dwError);
    goto cleanup;
}

/*
 * Delete the whole subtree in DIT
 * BUGBUG, need to handle big subtree scenario.
 */
DWORD
VmDirDeleteDITSubtree(
   LDAP*    pLD,
   PCSTR    pszDN
   )
{
    DWORD           dwError = 0;
    LDAPMessage*    pSearchRes = NULL;
    LDAPMessage*    pEntry = NULL;
    PSTR            pszEntryDN = NULL;
    int             iSize = 0;
    int             iCnt = 0;
    int             iDeleted = 0;
    PSTR*           ppDNArray = NULL;

    if ( !pLD || !pszDN || pszDN[0]=='\0' )
    {
        dwError = VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = ldap_search_ext_s(
                                 pLD,
                                 pszDN,
                                 LDAP_SCOPE_SUBTREE,
                                 NULL,              /* filter */
                                 NULL,              /* attrs[]*/
                                 FALSE,
                                 NULL,              /* serverctrls */
                                 NULL,              /* clientctrls */
                                 NULL,              /* timeout */
                                 0,
                                 &pSearchRes);
    BAIL_ON_VMDIR_ERROR(dwError);

    iSize = ldap_count_entries( pLD, pSearchRes );
    dwError = VmDirAllocateMemory( sizeof(PSTR)*iSize, (PVOID*)&ppDNArray );
    BAIL_ON_VMDIR_ERROR(dwError);

    for ( pEntry = ldap_first_entry(pLD, pSearchRes), iCnt = 0;
          pEntry;
          pEntry = ldap_next_entry(pLD, pEntry), iCnt++)
    {
        pszEntryDN = ldap_get_dn(pLD, pEntry);
        dwError = VmDirAllocateStringA( pszEntryDN, &(ppDNArray[iCnt]) );
        BAIL_ON_VMDIR_ERROR(dwError);

        ldap_memfree( pszEntryDN );
        pszEntryDN = NULL;
    }

    // sort DN array by DN length
    qsort ( ppDNArray, iSize, sizeof( PSTR ), VmDirCompareStrByLen );

    // delete DN (order by length)
    for ( iCnt = iSize - 1, iDeleted = 0; iCnt >= 0; iCnt-- )
    {
        dwError = ldap_delete_ext_s( pLD, ppDNArray[iCnt], NULL, NULL );
        switch (dwError)
        {
            case LDAP_NO_SUCH_OBJECT:
                VMDIR_LOG_VERBOSE( VMDIR_LOG_MASK_ALL, "Tried deleting (%s), No Such Object.", ppDNArray[iCnt]);
                dwError = LDAP_SUCCESS;
                break;

            case LDAP_SUCCESS:
                iDeleted++;
                VMDIR_LOG_VERBOSE( VMDIR_LOG_MASK_ALL, "(%s) deleted successfully.", ppDNArray[iCnt]);
                break;

            default:
                VMDIR_LOG_VERBOSE( VMDIR_LOG_MASK_ALL, "(%s) deletion failed, error (%d).", ppDNArray[iCnt], dwError);
                break;
        }
    }

    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "VmDirDeleteSubtree: subtree (%s) (%u) deleted", pszDN, iDeleted);

cleanup:

    for (iCnt = 0; iCnt < iSize; iCnt++)
    {
        VMDIR_SAFE_FREE_MEMORY( ppDNArray[iCnt] );
    }
    VMDIR_SAFE_FREE_MEMORY(ppDNArray);

    ldap_memfree( pszEntryDN );
    ldap_msgfree( pSearchRes );

    return dwError;

error:

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirDeleteSubtree: subtree (%s) delete failed (%u)", pszDN, dwError);
    goto cleanup;
}

/*
 * Delete DC Account
 */
DWORD
VmDirLdapDeleteDCAccount(
    LDAP *pLd,
    PCSTR   pszDomainName,
    PCSTR   pszDCHostName,             // Self host name
    BOOLEAN bActuallyDelete
    )
{
    DWORD       dwError = 0;
    PSTR        pszDCDN = NULL;
    PSTR        pszDomainDN = NULL;
    PSTR        pszLowerCaseDCHostName = NULL;

    dwError = VmDirAllocASCIIUpperToLower( pszDCHostName, &pszLowerCaseDCHostName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszDCDN, "%s=%s,%s=%s,%s", ATTR_CN, pszLowerCaseDCHostName,
                                             ATTR_OU, VMDIR_DOMAIN_CONTROLLERS_RDN_VAL, pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (bActuallyDelete)
    {
        dwError = ldap_delete_ext_s(pLd, pszDCDN, NULL, NULL);
        switch (dwError)
        {
            case LDAP_NO_SUCH_OBJECT:
                VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "Tried deleting DC account object (%s), No Such Object.", pszDCDN);
                dwError = LDAP_SUCCESS;
                break;

            case LDAP_SUCCESS:
                VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "DC account (%s) deleted", pszDCDN);
                break;

            default:
                VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "DC account object (%s) deletion failed, error (%d).", pszDCDN,
                                dwError);
                break;
        }
        BAIL_ON_VMDIR_ERROR(dwError);
    }
    else
    {
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "Would have deleted DC account (%s)", pszDCDN);
    }

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszDCDN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pszLowerCaseDCHostName);

    return dwError;

error:
    VMDIR_LOG_ERROR( VMDIR_LOG_MASK_ALL, "VmDirLdapDeleteDCAccount (%s) failed with error (%u)",
                     VDIR_SAFE_STRING(pszDCDN), dwError);
    goto cleanup;
}

/*
 * Delete a service account, that belongs to this Domain Controller
 */
DWORD
VmDirLdapDeleteServiceAccount(
    LDAP    *pLd,
    PCSTR   pszDomainName,
    PCSTR   pszServiceName,
    PCSTR   pszDCHostName,         // Self host name
    BOOLEAN bActuallyDelete
    )
{
    DWORD       dwError = 0;
    PSTR        pszMSADN = NULL;
    PSTR        pszDomainDN = NULL;
    PSTR        pszUPN = NULL;
    PSTR        pszUpperCaseDomainName = NULL;
    PSTR        pszLowerCaseDCHostName = NULL;

    dwError = VmDirAllocASCIILowerToUpper( pszDomainName, &pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocASCIIUpperToLower( pszDCHostName, &pszLowerCaseDCHostName);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszUPN, "%s/%s@%s", pszServiceName, pszLowerCaseDCHostName, pszUpperCaseDomainName );
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateDomainDN(pszDomainName, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringAVsnprintf( &pszMSADN, "%s=%s,%s=%s,%s", ATTR_CN, pszUPN,
                                             ATTR_CN, VMDIR_MSAS_RDN_VAL, pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (bActuallyDelete)
    {
        dwError = ldap_delete_ext_s(pLd, pszMSADN, NULL, NULL);
        switch (dwError)
        {
            case LDAP_NO_SUCH_OBJECT:
                VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "Tried deleting Service account object (%s), No Such Object.",
                                pszMSADN);
                dwError = LDAP_SUCCESS;
                break;

            case LDAP_SUCCESS:
                VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "Service account (%s) deleted", pszMSADN);
                break;

            default:
                VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "Service account object (%s) deletion failed, error (%d).", pszMSADN,
                                dwError);
                break;
        }
        BAIL_ON_VMDIR_ERROR(dwError);
    }
    else
    {
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "Would have deleted the service account (%s)", pszMSADN);
    }

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszMSADN);
    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);
    VMDIR_SAFE_FREE_MEMORY(pszUPN);
    VMDIR_SAFE_FREE_MEMORY(pszUpperCaseDomainName);
    VMDIR_SAFE_FREE_MEMORY(pszLowerCaseDCHostName);

    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapDeleteServiceAccount (%s) failed with error (%u)",
                    VDIR_SAFE_STRING(pszMSADN), dwError);
    goto cleanup;
}

/*
 * Add an account to a builtin group
 */
static
DWORD
_VmDirLdapSetupAccountMembership(
    LDAP*   pLd,
    PCSTR   pszDomainDN,
    PCSTR   pszBuiltinGroupName,
    PCSTR   pszAccountDN
    )
{
    DWORD       dwError = 0;
    LDAPMod     mod = {0};
    LDAPMod*    mods[2] = {&mod, NULL};
    PSTR        vals[2] = {(PSTR)pszAccountDN, NULL};
    PSTR        pszGroupDN = NULL;

    // set DomainControllerGroupDN
    dwError = VmDirAllocateStringAVsnprintf( &pszGroupDN,
                                             "cn=%s,cn=%s,%s",
                                             pszBuiltinGroupName,
                                             VMDIR_BUILTIN_CONTAINER_NAME,
                                             pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    mod.mod_op = LDAP_MOD_ADD;
    mod.mod_type = (PSTR)ATTR_MEMBER;
    mod.mod_vals.modv_strvals = vals;

    dwError = ldap_modify_ext_s(
                            pLd,
                            pszGroupDN,
                            mods,
                            NULL,
                            NULL);
    if ( dwError == LDAP_TYPE_OR_VALUE_EXISTS )
    {
        dwError = 0;    // already a member of group
    }
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirLdapSetupAccountMembership (%s)", pszAccountDN);

cleanup:

    VMDIR_SAFE_FREE_STRINGA(pszGroupDN);

    return dwError;

error:

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirLdapSetupAccountMembership failed. Error(%u)", dwError);

    goto cleanup;
}

/*
 * query single attribute value of a DN via ldap
 * "*ppByte" is NULL terminated.
 */
DWORD
VmDirLdapGetSingleAttribute(
    LDAP*   pLD,
    PCSTR   pszDN,
    PCSTR   pszAttr,
    PBYTE*  ppByte,
    DWORD*  pdwLen
    )
{
    DWORD           dwError=0;
    PBYTE           pLocalByte = NULL;
    BerValue**      ppBerValues = NULL;

    if ( !pLD || !pszDN || !pszAttr || !ppByte || !pdwLen )
    {
        dwError = VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirLdapGetAttributeValues( pLD,
                                           pszDN,
                                           pszAttr,
                                           NULL,
                                           &ppBerValues);
    BAIL_ON_VMDIR_ERROR(dwError);

    if ( ppBerValues[0] == NULL || ppBerValues[0]->bv_val == NULL )
    {
        dwError = VMDIR_ERROR_NO_SUCH_ATTRIBUTE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if ( ppBerValues[1] != NULL )   // more than one attribute value
    {
        dwError = VMDIR_ERROR_INVALID_RESULT;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirAllocateAndCopyMemory(
                            ppBerValues[0]->bv_val,
                            ppBerValues[0]->bv_len + 1,
                            (PVOID*)&pLocalByte);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppByte = pLocalByte;
    *pdwLen = (DWORD)ppBerValues[0]->bv_len;
    pLocalByte = NULL;

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pLocalByte);
    if(ppBerValues)
    {
        ldap_value_free_len(ppBerValues);
    }

    return dwError;

error:

    goto cleanup;
}

/*
 * reset attribute value via ldap
 */
DWORD
VmDirLdapModReplaceAttribute(
    LDAP*   pLd,
    PCSTR   pszDN,
    PCSTR   pszAttribute,
    PCSTR   pszValue
    )
{
    DWORD       dwError = 0;
    LDAPMod     mod = {0};
    LDAPMod*    mods[2] = {&mod, NULL};
    PSTR        vals[2] = {(PSTR)pszValue, NULL};

    mod.mod_op = LDAP_MOD_REPLACE;
    mod.mod_type = (PSTR)pszAttribute;
    mod.mod_vals.modv_strvals = vals;

    dwError = ldap_modify_ext_s(
                            pLd,
                            pszDN,
                            mods,
                            NULL,
                            NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirLdapModReplaceAttribute (%s)(%s)", pszDN, pszAttribute);

cleanup:

    return dwError;

error:

    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirLdapModReplaceAttribute failed. Error(%u)", dwError);

    goto cleanup;
}

DWORD
VmDirGetServerAccountDN(
    PCSTR pszDomain,
    PCSTR pszMachineName,
    PSTR* ppszServerDN
    )
{
    DWORD dwError = 0;
    PSTR  pszDomainDN = NULL;
    PSTR  pszServerDN = NULL;

    if (IsNullOrEmptyString(pszDomain) ||
        IsNullOrEmptyString(pszMachineName) ||
        !ppszServerDN)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringPrintf(
                    &pszServerDN,
                    "CN=%s,OU=%s,%s",
                    pszMachineName,
                    VMDIR_DOMAIN_CONTROLLERS_RDN_VAL,
                    pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszServerDN = pszServerDN;

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    return dwError;

error:

    if (ppszServerDN)
    {
        *ppszServerDN = NULL;
    }

    VMDIR_SAFE_FREE_MEMORY(pszServerDN);

    goto cleanup;
}

DWORD
VmDirGetServerGuidInternal(
    LDAP* pLd,
    PCSTR pszDomain,
    PCSTR pszMachineName,
    PSTR* ppszGUID
    )
{
    DWORD dwError = 0;
    PSTR  pszFilter = "(objectclass=computer)";
    PSTR  pszAccountDN = NULL;
    PSTR  pszAttrMachineGUID = ATTR_VMW_MACHINE_GUID;
    PSTR  ppszAttrs[] = { pszAttrMachineGUID, NULL };
    LDAPMessage *pResult = NULL;
    LDAPMessage *pEntry = NULL;
    struct berval** ppValues = NULL;
    PSTR  pszGUID = NULL;

    dwError = VmDirGetServerAccountDN(pszDomain, pszMachineName, &pszAccountDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = ldap_search_ext_s(
                    pLd,
                    pszAccountDN,
                    LDAP_SCOPE_BASE,
                    pszFilter,
                    ppszAttrs,
                    FALSE, /* get values      */
                    NULL,  /* server controls */
                    NULL,  /* client controls */
                    NULL,  /* timeout         */
                    0,     /* size limit      */
                    &pResult);
    BAIL_ON_VMDIR_ERROR(dwError);

    //searching by name should yield just one
    if (ldap_count_entries(pLd, pResult) != 1)
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    pEntry = ldap_first_entry(pLd, pResult);
    if (!pEntry)
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    ppValues = ldap_get_values_len(pLd, pEntry, pszAttrMachineGUID);

    if (!ppValues || (ldap_count_values_len(ppValues) != 1))
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirAllocateStringPrintf(
                        &pszGUID,
                        "%s",
                        ppValues[0]->bv_val);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszGUID = pszGUID;

cleanup:

    if (ppValues)
    {
        ldap_value_free_len(ppValues);
    }

    if (pResult)
    {
        ldap_msgfree(pResult);
    }

    VMDIR_SAFE_FREE_MEMORY(pszAccountDN);

    return dwError;

error:

    if (ppszGUID)
    {
        *ppszGUID = NULL;
    }

    VMDIR_SAFE_FREE_MEMORY(pszGUID);

    goto cleanup;
}

DWORD
VmDirSetServerGuidInternal(
    LDAP* pLd,
    PCSTR pszDomain,
    PCSTR pszMachineName,
    PCSTR pszGUID
    )
{
    DWORD dwError = 0;
    PCSTR pszAttrObjectGUID = ATTR_VMW_MACHINE_GUID;
    PSTR  pszAccountDN = NULL;

    dwError = VmDirGetServerAccountDN(pszDomain, pszMachineName, &pszAccountDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapModReplaceAttribute(
                    pLd,
                    pszAccountDN,
                    pszAttrObjectGUID,
                    pszGUID);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pszAccountDN);

    return dwError;

error:

    goto cleanup;
}

DWORD
VmDirGetComputerAccountDN(
    PCSTR pszDomain,
    PCSTR pszMachineName,
    PSTR* ppszAccountDN
    )
{
    DWORD dwError = 0;
    PSTR  pszDomainDN = NULL;
    PSTR  pszAccountDN = NULL;

    if (IsNullOrEmptyString(pszDomain) ||
        IsNullOrEmptyString(pszMachineName) ||
        !ppszAccountDN)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirSrvCreateDomainDN(pszDomain, &pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateStringPrintf(
                    &pszAccountDN,
                    "CN=%s,OU=%s,%s",
                    pszMachineName,
                    VMDIR_COMPUTERS_RDN_VAL,
                    pszDomainDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszAccountDN = pszAccountDN;

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pszDomainDN);

    return dwError;

error:

    if (ppszAccountDN)
    {
        *ppszAccountDN = NULL;
    }

    VMDIR_SAFE_FREE_MEMORY(pszAccountDN);

    goto cleanup;
}

DWORD
VmDirGetComputerGuidInternal(
    LDAP* pLd,
    PCSTR pszDomain,
    PCSTR pszMachineName,
    PSTR* ppszGUID
    )
{
    DWORD dwError = 0;
    PSTR  pszFilter = "(objectclass=computer)";
    PSTR  pszAccountDN = NULL;
    PSTR  pszAttrMachineGUID = ATTR_VMW_MACHINE_GUID;
    PSTR  ppszAttrs[] = { pszAttrMachineGUID, NULL };
    LDAPMessage *pResult = NULL;
    LDAPMessage *pEntry = NULL;
    struct berval** ppValues = NULL;
    PSTR  pszGUID = NULL;

    dwError = VmDirGetComputerAccountDN(
                pszDomain,
                pszMachineName,
                &pszAccountDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = ldap_search_ext_s(
                    pLd,
                    pszAccountDN,
                    LDAP_SCOPE_BASE,
                    pszFilter,
                    ppszAttrs,
                    FALSE, /* get values      */
                    NULL,  /* server controls */
                    NULL,  /* client controls */
                    NULL,  /* timeout         */
                    0,     /* size limit      */
                    &pResult);
    BAIL_ON_VMDIR_ERROR(dwError);

    //searching by name should yield just one
    if (ldap_count_entries(pLd, pResult) != 1)
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    pEntry = ldap_first_entry(pLd, pResult);
    if (!pEntry)
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    ppValues = ldap_get_values_len(pLd, pEntry, pszAttrMachineGUID);

    if (!ppValues || (ldap_count_values_len(ppValues) != 1))
    {
        dwError = ERROR_INVALID_STATE;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirAllocateStringPrintf(
                        &pszGUID,
                        "%s",
                        ppValues[0]->bv_val);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszGUID = pszGUID;

cleanup:

    if (ppValues)
    {
        ldap_value_free_len(ppValues);
    }

    if (pResult)
    {
        ldap_msgfree(pResult);
    }

    VMDIR_SAFE_FREE_MEMORY(pszAccountDN);

    return dwError;

error:

    if (ppszGUID)
    {
        *ppszGUID = NULL;
    }

    VMDIR_SAFE_FREE_MEMORY(pszGUID);

    goto cleanup;
}

DWORD
VmDirSetComputerGuidInternal(
    LDAP* pLd,
    PCSTR pszDomain,
    PCSTR pszMachineName,
    PCSTR pszGUID
    )
{
    DWORD dwError = 0;
    PCSTR pszAttrObjectGUID = ATTR_VMW_MACHINE_GUID;
    PSTR  pszAccountDN = NULL;

    dwError = VmDirGetComputerAccountDN(
                pszDomain,
                pszMachineName,
                &pszAccountDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirLdapModReplaceAttribute(
                    pLd,
                    pszAccountDN,
                    pszAttrObjectGUID,
                    pszGUID);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:

    VMDIR_SAFE_FREE_MEMORY(pszAccountDN);

    return dwError;

error:

    goto cleanup;
}

DWORD
VmDirGetMemberships(
    PVMDIR_CONNECTION pConnection,
    PCSTR pszUPNName,
    PSTR  **pppszMemberships,
    PDWORD pdwMemberships
    )
{
    DWORD dwError = 0;
    PSTR pszFilter = NULL;
    PSTR pszAttrMemberOf = ATTR_MEMBEROF; // memberOf
    PSTR  ppszAttrs[] = { pszAttrMemberOf, NULL};
    DWORD dwCount = 0;
    LDAPMessage *pResult = NULL;
    LDAPMessage *pEntry = NULL;
    struct berval** ppValues = NULL;
    PSTR *ppszMemberships = NULL;
    DWORD dwMemberships = 0;
    DWORD i = 0;

    if (pConnection == NULL ||
        pConnection->pLd == NULL ||
        IsNullOrEmptyString(pszUPNName) ||
        pppszMemberships == NULL ||
        pdwMemberships == NULL)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirAllocateStringPrintf(&pszFilter, "(%s=%s)", ATTR_KRB_UPN, pszUPNName); // userPrincipalName
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = ldap_search_ext_s(
                    pConnection->pLd,
                    "",
                    LDAP_SCOPE_SUBTREE,
                    pszFilter,
                    (PSTR*)ppszAttrs,
                    0,
                    NULL,
                    NULL,
                    NULL,
                    -1,
                    &pResult);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwCount = ldap_count_entries(pConnection->pLd, pResult);
    if (dwCount == 0)
    {
        dwError = LDAP_NO_SUCH_OBJECT;
        BAIL_ON_VMDIR_ERROR(dwError);
    }
    else if (dwCount > 1)
    {
        dwError = LDAP_OPERATIONS_ERROR;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    pEntry = ldap_first_entry(pConnection->pLd, pResult);
    if (!pEntry)
    {
        dwError = LDAP_NO_SUCH_OBJECT;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    ppValues = ldap_get_values_len(pConnection->pLd, pEntry, pszAttrMemberOf);
    if (!ppValues)
    {
        dwMemberships = 0;
    }
    else
    {
        dwMemberships = ldap_count_values_len(ppValues);
    }

    if (dwMemberships)
    {
        dwError = VmDirAllocateMemory(dwMemberships * sizeof(PSTR), (PVOID)&ppszMemberships);
        BAIL_ON_VMDIR_ERROR(dwError);

        for (i = 0; ppValues[i] != NULL; i++)
        {
            PCSTR pszMemberOf = ppValues[i]->bv_val;

            dwError = VmDirAllocateStringA(pszMemberOf, &ppszMemberships[i]);
            BAIL_ON_VMDIR_ERROR(dwError);
        }
    }

    *pppszMemberships = ppszMemberships;
    *pdwMemberships = dwMemberships;

cleanup:

    if(ppValues)
    {
        ldap_value_free_len(ppValues);
    }

    if (pResult)
    {
        ldap_msgfree(pResult);
    }

    VMDIR_SAFE_FREE_MEMORY(pszFilter);

    return dwError;

error:
    if (ppszMemberships != NULL && dwMemberships > 0)
    {
        for (i = 0; i < dwMemberships; i++)
        {
            VMDIR_SAFE_FREE_STRINGA(ppszMemberships[i]);
        }
        VMDIR_SAFE_FREE_MEMORY(ppszMemberships);
    }
    goto cleanup;
}

