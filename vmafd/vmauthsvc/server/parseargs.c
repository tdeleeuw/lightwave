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

#ifndef _WIN32

DWORD
VmAuthsvcParseArgs(
    int argc,
    char* argv[],
    int* pLoggingLevel,
    PCSTR* ppszLogFileName,
    int* pLdapPort,
    PBOOLEAN pbEnableSysLog
)
{
    DWORD dwError = ERROR_SUCCESS;
    int opt = 0;
    setlocale(LC_ALL, "");

    //TODO, change to use long opt
    while ( (opt = getopt( argc, argv, VMAUTHSVC_OPTIONS_VALID)) != EOF )
    {
        switch ( opt )
        {
            case VMAUTHSVC_OPTION_LOGGING_LEVEL:
                if( pLoggingLevel != NULL )
                {
                    *pLoggingLevel = atoi( optarg );
                }
                break;

            case VMAUTHSVC_OPTION_LOG_FILE_NAME:
                if( ppszLogFileName != NULL )
                {
                    *ppszLogFileName = optarg;
                }
                break;

            case VMAUTHSVC_OPTION_PORT:
                if( pLdapPort != NULL )
                {
                    *pLdapPort = atoi( optarg );
                }
                break;

            case VMAUTHSVC_OPTION_ENABLE_SYSLOG:
                if ( pbEnableSysLog != NULL )
                {
                    *pbEnableSysLog = TRUE;
                }
                break;

            default:
                dwError = ERROR_INVALID_PARAMETER;
                BAIL_ON_VMAUTHSVC_ERROR(dwError);
        }
    }
error:
    return dwError;
}

#else

DWORD
VmAuthsvcParseArgs(
    int argc,
    char* argv[],
    int* pLoggingLevel,
    PCSTR* ppszLogFileName,
    int* pLdapPort,
    PBOOLEAN pbEnableSysLog
)
{
    DWORD dwError = ERROR_SUCCESS;
    int i = 1; // first arg is the <name of exe>.exe

    while( i < argc )
    {
        if( VmAuthsvcIsCmdLineOption( argv[i] ) != FALSE )
        {
            if ( VmAuthsvcStringCompareA(
                          VMAUTHSVC_OPTION_LOGGING_LEVEL, argv[i], TRUE ) == 0 )
            {
                dwError = VmAuthsvcGetCmdLineIntOption(
                    argc, argv, &i, pLoggingLevel
                );
                BAIL_ON_VMAUTHSVC_ERROR(dwError);
            }
            else if ( VmAuthsvcStringCompareA(
                          VMAUTHSVC_OPTION_LOG_FILE_NAME, argv[i], TRUE ) == 0 )
            {
                VmAuthsvcGetCmdLineOption( argc, argv, &i, ppszLogFileName );
            }
            else if ( VmAuthsvcStringCompareA(
                          VMAUTHSVC_OPTION_PORT, argv[i], TRUE ) == 0 )
            {
                dwError = VmAuthsvcGetCmdLineIntOption(
                    argc, argv, &i, pLdapPort
                );
                BAIL_ON_VMAUTHSVC_ERROR(dwError);
            }
            else if ( VmAuthsvcStringCompareA(
                          VMAUTHSVC_OPTION_ENABLE_SYSLOG, argv[i], TRUE ) == 0 )
            {
                if ( pbEnableSysLog != NULL )
                {
                    *pbEnableSysLog = TRUE;
                }
            }
            else
            {
                dwError = ERROR_INVALID_PARAMETER;
                BAIL_ON_VMAUTHSVC_ERROR(dwError);
            }
        }

        i++;
    } // while

error:

    return dwError;
}

#endif

VOID
ShowUsage(
    PSTR pName
)
{
    //TODO, cleanup after use long opt
   fprintf(
       stderr,
       "Usage: %s [-d <logging level (an integer)>] [-p <ldap port>]",
       pName
   );
}
