/*
 * Copyright © 2017 VMware, Inc.  All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the “License”); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS, without
 * warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 * License for the specific language governing permissions and limitations
 */

typedef struct _VDIR_EVENT_REPO
{
    PVDIR_LINKED_LIST   pReadyEventList;
    PVDIR_QUEUE         pPendingQueue;
}VDIR_EVENT_REPO, *PVDIR_EVENT_REPO;

typedef PVDIR_EVENT PVDIR_EVENT_REPO_COOKIE;

typedef struct _VDIR_WATCH_SESSION
{
    BOOL                    bPrevVersion;
    DWORD                   watchSessionId;
    DWORD                   startRevision;
    PSTR                    pszFilter;
    PVOID                   pConnectionHndl;
    VDIR_BERVALUE           subTreeDn;
    PVDIR_EVENT_REPO        pEventRepo;
    PVDIR_EVENT_REPO_COOKIE pRepoCookie;
}VDIR_WATCH_SESSION, *PVDIR_WATCH_SESSION;

typedef struct _VDIR_WATCH_SESSION_MANAGER
{
    DWORD               nextWatchId;
    PLW_HASHMAP         pDeletedMap;
    PVDIR_QUEUE         pActiveQueue;
    PVDIR_QUEUE         pInactiveQueue;
    PVDIR_EVENT_REPO    pEventRepo;
    PVMDIR_MUTEX        pMutex;
}VDIR_WATCH_SESSION_MANAGER, *PVDIR_WATCH_SESSION_MANAGER;