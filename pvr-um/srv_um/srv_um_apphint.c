/* SPDX-License-Identifier: MIT */
/*
 * Application hints read from powervr.ini files.
 *
 * File grammar: a line "[name]" opens a section that is active when name is
 * the process name (basename of the first word of /proc/self/cmdline) or
 * "default". Inside an active section a line "Hint = value" whose
 * alphanumeric name equals the requested hint assigns the value. A value
 * from the process-name section ends the search; a value from the default
 * section is kept while the search continues, so a later process-name
 * section overrides it. A string value is the rest of the line without the
 * blanks after '=' and without trailing blanks or the '\r' of a CRLF line
 * ending; other values are the first blank-delimited word after '='.
 */
#include "srv_um_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRV_APPHINT_MAX_LEN	(APPHINT_MAX_STRING_SIZE - 1)
#define SRV_APPHINT_SECTION_MAX	256

/* Modules that read hints from the ini files: EGL, OpenGL ES 1 and 2,
 * services UM, OpenVG, services client, OpenGL, Android HAL, OpenCL
 * (IMG_MODULE_ID values 1, 2, 3, 5, 6, 7, 12, 14, 15). */
#define SRV_APPHINT_MODULE_MASK	0x6877u

typedef struct SRV_APPHINT_STATE_TAG
{
	IMG_MODULE_ID	eModuleID;
	const IMG_CHAR	*pszAppName;
} SRV_APPHINT_STATE;

typedef enum
{
	SRV_SECTION_NONE,
	SRV_SECTION_INACTIVE,
	SRV_SECTION_DEFAULT,
	SRV_SECTION_MATCH
} SRV_SECTION;

IMG_EXPORT IMG_VOID PVRSRVCreateAppHintState(IMG_MODULE_ID eModuleID,
					     const IMG_CHAR *pszAppName,
					     IMG_VOID **ppvState)
{
	SRV_APPHINT_STATE *psState = malloc(sizeof(*psState));

	if (psState == NULL)
	{
		SRV_ERR("PVRSRVCreateAppHintState: out of memory");
	}
	else
	{
		psState->eModuleID = eModuleID;
		psState->pszAppName = pszAppName;
	}
	*ppvState = psState;
}

IMG_EXPORT IMG_VOID PVRSRVFreeAppHintState(IMG_MODULE_ID eModuleID, IMG_VOID *pvHintState)
{
	(void)eModuleID;
	free(pvHintState);
}

/* Reads the basename of argv[0] from /proc/self/cmdline into pszName. */
static IMG_BOOL SrvAppName(IMG_CHAR *pszName, size_t uiSize)
{
	char acCmd[SRV_APPHINT_MAX_LEN + 1];
	const char *pszBase;
	size_t uiLen;
	FILE *psFile = fopen("/proc/self/cmdline", "re");

	if (psFile == NULL)
	{
		SRV_ERR("SrvAppName: cannot open /proc/self/cmdline");
		return IMG_FALSE;
	}
	uiLen = fread(acCmd, 1, sizeof(acCmd) - 1, psFile);
	(void)fclose(psFile);
	if (uiLen == 0)
	{
		SRV_ERR("SrvAppName: cannot read /proc/self/cmdline");
		return IMG_FALSE;
	}
	acCmd[uiLen] = '\0';

	pszBase = strrchr(acCmd, '/');
	pszBase = (pszBase != NULL) ? pszBase + 1 : acCmd;
	(void)snprintf(pszName, uiSize, "%s", pszBase);
	return IMG_TRUE;
}

static IMG_BOOL SrvConvertHint(const char *pszValue, IMG_DATA_TYPE eDataType,
			       IMG_VOID *pvReturn)
{
	switch (eDataType)
	{
		case IMG_STRING_TYPE:
			(void)snprintf(pvReturn, APPHINT_MAX_STRING_SIZE, "%s", pszValue);
			return IMG_TRUE;
		case IMG_FLOAT_TYPE:
			*(IMG_FLOAT *)pvReturn = (IMG_FLOAT)strtod(pszValue, NULL);
			return IMG_TRUE;
		case IMG_UINT_TYPE:
		case IMG_FLAG_TYPE:
			*(IMG_UINT32 *)pvReturn = (IMG_UINT32)strtoul(pszValue, NULL, 0);
			return IMG_TRUE;
		case IMG_INT_TYPE:
			*(IMG_INT32 *)pvReturn = (IMG_INT32)strtol(pszValue, NULL, 10);
			return IMG_TRUE;
		default:
			SRV_ERR("SrvConvertHint: Bad eDataType %d", (int)eDataType);
			return IMG_FALSE;
	}
}

static SRV_SECTION SrvParseSection(const char *pszLine, const char *pszApp,
				   const char *pszFile, unsigned uLine,
				   SRV_SECTION eCurrent)
{
	const char *pszEnd = strchr(pszLine + 1, ']');
	size_t uiLen;

	if (pszEnd == NULL)
	{
		SRV_ERR("%s:%u: section name is not terminated by ']'", pszFile, uLine);
		return eCurrent;
	}
	uiLen = (size_t)(pszEnd - (pszLine + 1));
	if (uiLen > SRV_APPHINT_SECTION_MAX)
	{
		SRV_ERR("%s:%u: section name too long", pszFile, uLine);
		return eCurrent;
	}
	if (strlen(pszApp) == uiLen && strncmp(pszLine + 1, pszApp, uiLen) == 0)
	{
		return SRV_SECTION_MATCH;
	}
	if (uiLen == 7 && strncmp(pszLine + 1, "default", 7) == 0)
	{
		return SRV_SECTION_DEFAULT;
	}
	return SRV_SECTION_INACTIVE;
}

/*
 * Parses one "Name = value" line of an active section. Returns 1 when the
 * name equals pszHintName and *ppszValue points at the value text after the
 * blanks that follow '=' (ending before trailing blanks and '\r' for
 * strings, at the first blank for other types), 0 for
 * a well-formed line naming another hint, -1 for a malformed line.
 */
static int SrvParseAssignment(char *pszLine, const char *pszHintName,
			      IMG_DATA_TYPE eDataType, char **ppszValue)
{
	char *pszName = pszLine;
	char *pszCursor;
	char *pszValue;
	size_t uiNameLen;

	while (*pszName == ' ' || *pszName == '\t')
	{
		pszName++;
	}
	pszCursor = pszName;
	while (isalnum((unsigned char)*pszCursor))
	{
		pszCursor++;
	}
	uiNameLen = (size_t)(pszCursor - pszName);
	if (uiNameLen == 0 || uiNameLen > SRV_APPHINT_MAX_LEN)
	{
		return -1;
	}
	while (*pszCursor == ' ' || *pszCursor == '\t')
	{
		pszCursor++;
	}
	if (*pszCursor != '=')
	{
		return -1;
	}
	pszValue = pszCursor + 1;
	while (*pszValue == ' ' || *pszValue == '\t')
	{
		pszValue++;
	}

	if (eDataType == IMG_STRING_TYPE)
	{
		size_t uiLen = strlen(pszValue);

		while (uiLen > 0 && (pszValue[uiLen - 1] == ' ' || pszValue[uiLen - 1] == '\t' ||
				     pszValue[uiLen - 1] == '\r'))
		{
			uiLen--;
		}
		pszValue[uiLen] = '\0';
	}
	else
	{
		char *pszStop = pszValue;

		while (*pszStop != '\0' && !isspace((unsigned char)*pszStop))
		{
			pszStop++;
		}
		*pszStop = '\0';
	}
	if (*pszValue == '\0' || strlen(pszValue) > SRV_APPHINT_MAX_LEN)
	{
		return -1;
	}

	pszName[uiNameLen] = '\0';
	if (strcmp(pszName, pszHintName) != 0)
	{
		return 0;
	}
	*ppszValue = pszValue;
	return 1;
}

static IMG_BOOL SrvFindAppHintInFile(const char *pszFile, const char *pszApp,
				     const char *pszHintName, IMG_DATA_TYPE eDataType,
				     IMG_VOID *pvReturn)
{
	SRV_SECTION eSection = SRV_SECTION_NONE;
	IMG_BOOL bResult = IMG_FALSE;
	char *pszLine = NULL;
	size_t uiCap = 0;
	unsigned uLine = 0;
	ssize_t iLen;
	FILE *psFile = fopen(pszFile, "re");

	if (psFile == NULL)
	{
		return IMG_FALSE;
	}

	while ((iLen = getline(&pszLine, &uiCap, psFile)) >= 0)
	{
		char *pszValue = NULL;
		int iParse;

		uLine++;
		if (iLen > 0 && pszLine[iLen - 1] == '\n')
		{
			pszLine[iLen - 1] = '\0';
		}

		if (pszLine[0] == '[')
		{
			eSection = SrvParseSection(pszLine, pszApp, pszFile, uLine, eSection);
			continue;
		}
		if (eSection != SRV_SECTION_DEFAULT && eSection != SRV_SECTION_MATCH)
		{
			continue;
		}
		if (strspn(pszLine, " \t\r") == strlen(pszLine))
		{
			continue;
		}

		iParse = SrvParseAssignment(pszLine, pszHintName, eDataType, &pszValue);
		if (iParse < 0)
		{
			SRV_ERR("%s:%u: malformed hint line", pszFile, uLine);
			continue;
		}
		if (iParse == 0)
		{
			continue;
		}

		bResult = SrvConvertHint(pszValue, eDataType, pvReturn);
		if (bResult && eSection == SRV_SECTION_MATCH)
		{
			break;
		}
	}

	free(pszLine);
	(void)fclose(psFile);
	return bResult;
}

IMG_EXPORT IMG_BOOL PVRSRVGetAppHint(IMG_VOID *pvHintState,
				     const IMG_CHAR *pszHintName,
				     IMG_DATA_TYPE eDataType,
				     const IMG_VOID *pvDefault,
				     IMG_VOID *pvReturn)
{
	const SRV_APPHINT_STATE *psState = pvHintState;

	if (psState != NULL && pszHintName != NULL &&
	    psState->eModuleID >= 1 && psState->eModuleID <= 16 &&
	    ((SRV_APPHINT_MODULE_MASK >> ((unsigned)psState->eModuleID - 1u)) & 1u) != 0)
	{
		IMG_CHAR acApp[SRV_APPHINT_MAX_LEN + 1];

		if (SrvAppName(acApp, sizeof(acApp)))
		{
			IMG_BOOL bEtc = SrvFindAppHintInFile("/etc/powervr.ini", acApp,
							     pszHintName, eDataType, pvReturn);
			IMG_BOOL bLocal = SrvFindAppHintInFile("powervr.ini", acApp,
							       pszHintName, eDataType, pvReturn);

			if (bEtc || bLocal)
			{
				return IMG_TRUE;
			}
		}
	}

	if (eDataType == IMG_STRING_TYPE)
	{
		(void)snprintf(pvReturn, APPHINT_MAX_STRING_SIZE, "%s", (const char *)pvDefault);
	}
	else
	{
		memcpy(pvReturn, pvDefault, sizeof(IMG_UINT32));
	}
	return IMG_FALSE;
}
