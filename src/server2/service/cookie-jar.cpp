/*
 *  Copyright (c) 2000 - 2013 Samsung Electronics Co., Ltd All Rights Reserved
 *
 *  Contact: Bumjin Im <bj.im@samsung.com>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License
 */
/*
 * @file        cookie-jar.cpp
 * @author      Pawel Polawski (p.polawski@partner.samsung.com)
 * @version     1.0
 * @brief       This function contain implementation of CookieJar class which holds cookies structures
 */

#include <cookie-jar.h>
#include <protocols.h>
#include <dpl/log/log.h>
#include <dpl/exception.h>
#include <vector>
#include <stdbool.h>
#include <unistd.h>
#include <smack-check.h>
#include <privilege-control.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/smack.h>
#include <fstream>
#include <linux/limits.h>

namespace SecurityServer {

CookieJar::CookieJar(void)
{
    LogDebug("Created CookieJar for handling cookies");
}

CookieJar::~CookieJar(void)
{
    LogDebug("Deleted CookieJar");
}

const Cookie * CookieJar::GenerateCookie(int pid)
{
    char key[COOKIE_SIZE];
    int retval;

    LogDebug("Cookie creation called");

    //create empty cookie class
    Cookie newCookie;
    newCookie.pid = pid;

    //check if there is no cookie for specified PID
    const Cookie *searchResult = SearchCookie(newCookie, CompareType::PID);
    if (searchResult != NULL) {
        LogDebug("Cookie exist for specified PID");
        return searchResult;
    }

    searchResult = &newCookie;   //only for searchResult != NULL
    while(searchResult != NULL) {
        //generate unique key
        std::ifstream urandom("/dev/urandom", std::ifstream::binary);
        urandom.read(key, COOKIE_SIZE);
        newCookie.cookieId.assign(key, key + COOKIE_SIZE);

        //check if key is unique
        searchResult = SearchCookie(newCookie, CompareType::COOKIE_ID);
        if (searchResult != NULL)
            LogDebug("Key is not unique");
    }

    //obtain process path
    char link[PATH_MAX];
    char path[PATH_MAX];

    snprintf(link, PATH_MAX, "/proc/%d/exe", pid);
    retval = readlink(link, path, PATH_MAX);
    if (retval < 0) {
        LogDebug("Unable to get process path");
        return NULL;
    }
    path[retval] = '\0';
    newCookie.binaryPath = path;

    //get smack label if smack enabled
    if (smack_check()) {
        char label[SMACK_LABEL_LEN + 1];
        retval = get_smack_label_from_process(pid, label);
        if (retval != PC_OPERATION_SUCCESS) {
            LogDebug("Unable to get smack label of process");
            return NULL;
        }
        newCookie.smackLabel = label;
    } else
        newCookie.smackLabel = "smack_disabled";


    //get GID list
    const int LINE_LEN = 128;
    const int NAME_SIZE = 64;
    char line[LINE_LEN]; //for storing parsed lines
    char filename[NAME_SIZE];

    snprintf(filename, NAME_SIZE, "/proc/%d/status", pid);
    std::ifstream status(filename, std::ifstream::binary);

    while (status.getline(line, LINE_LEN)) {  //read line from file
        if (strncmp(line, "Groups:", 7) == 0)
            break;
    }

    char delim[] = ": ";    //separators for strtok: ' ' and ':'
    char *token = strtok(line, delim);  //1st string is "Group:"
    while ((token = strtok(NULL, delim))) {
        int gid = atoi(token);
        newCookie.permissions.push_back(gid);
    }

    //DEBUG ONLY
    //print info about cookie
    LogDebug("Cookie created");
    LogDebug("PID: " << newCookie.pid);
    LogDebug("PATH: " << newCookie.binaryPath);
    LogDebug("LABEL: " << newCookie.smackLabel);
    for (size_t k = 0; k < newCookie.permissions.size(); k++)
        LogDebug("GID: " << newCookie.permissions[k]);

    m_cookieList.push_back(newCookie);
    return &m_cookieList[m_cookieList.size() - 1];
}

void CookieJar::DeleteCookie(const Cookie &pattern, CompareType criterion)
{
    if (m_cookieList.size() == 0) {
        LogDebug("Cookie list empty");
        return;
    }

    //for each cookie in list
    for (size_t i = 0; i < m_cookieList.size();) {
        if (CompareCookies(pattern, m_cookieList[i], criterion)) {
            LogDebug("Deleting cookie");
            if (i != m_cookieList.size() - 1)
                m_cookieList[i] = *m_cookieList.rbegin();
            m_cookieList.pop_back();
        } else
            ++i;
    }
}

const Cookie * CookieJar::SearchCookie(const Cookie &pattern, CompareType criterion) const
{
    LogDebug("Searching for cookie");

    if (m_cookieList.size() == 0) {
        LogDebug("Cookie list empty");
        return NULL;
    }

    //for each cookie in list
    for (size_t i = 0; i < m_cookieList.size(); i++) {
        if (CompareCookies(pattern, m_cookieList[i], criterion)) {
            LogDebug("Cookie found");
            return &(m_cookieList[i]);
        }
    }

    LogDebug("Cookie not found");
    return NULL;
}

bool CookieJar::CompareCookies(const Cookie &c1, const Cookie &c2, CompareType criterion) const
{
    size_t permSize1 = c1.permissions.size();
    size_t permSize2 = c2.permissions.size();

    switch(criterion) {
    case CompareType::COOKIE_ID:
        return (c1.cookieId == c2.cookieId);

    case CompareType::PID:
        return (c1.pid == c2.pid);

    case CompareType::PATH:
        return (c1.binaryPath == c2.binaryPath);

    case CompareType::SMACKLABEL:
        return (c1.smackLabel == c2.smackLabel);

    case CompareType::PERMISSIONS:
        //we search for at least one the same GID
        for(size_t i = 0; i < permSize1; i++)
            for (size_t k = 0; k < permSize2; k++)
                if (c1.permissions[i] == c2.permissions[k])
                    return true;
        return false;

    default:
        LogDebug("Wrong function parameters");
        return false;
    };
}

} // namespace SecurityServer
