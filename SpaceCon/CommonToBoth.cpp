#include "stdafx.h"
#include <Windows.h>
#include <atlbase.h>

#include "CommonToBoth.h"

/* Global store of the current drive configuration data, 1 per drive letter */
CDriveCfg g_DriveConfig[26];

/* The event used to signal settings changes to any other running instances on other users desktops */
HANDLE g_hEvents[4];	/*	Event[0] is the signal to refresh
							Event[1] is the signal to exit the thread
							Event[2] is the signal to exit this instance (uninstalling)
							Event[3] is the signal to restart monitor instances.
						*/

void LoadGlobalSettingsFromReg() noexcept
{
	CRegKey rk;
	const LONG RegRes = rk.Open( HKEY_LOCAL_MACHINE, szRegistryKey, KEY_READ );

	if ( ERROR_SUCCESS == RegRes )
	{
		/* Load the global structure */
		DWORD Size = sizeof( g_DriveConfig );

		if ( ERROR_SUCCESS == rk.QueryBinaryValue( SETTINGS, &g_DriveConfig, &Size ) )
		{
			/* OK, they're read */
		}
		else
		{
			//			MessageBox( NULL, _T("Failed to read the values in the registry"), szAppName, MB_OK | MB_ICONINFORMATION );
		}
	}
	else
	{
		/* Failed to open/create the registry key. Probably don't have permission to do it - or it doesn't yet exist! */
#if 0	// Can't see the point in reporting this
		LPVOID lpMsgBuf;
		if ( FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			RegRes,
			MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ), // Default language
			(LPTSTR) &lpMsgBuf,
			0,
			NULL ) )
		{
			// Display the string.
			MessageBox( NULL, (LPCTSTR) lpMsgBuf, szAppName, MB_OK | MB_ICONINFORMATION );

			// Free the buffer.
			LocalFree( lpMsgBuf );
		}
		else
		{
			// Handle the error.
	//					return;
		}
#endif
	}
}

#define RESTRICTED_ACCESS_SD
#ifdef RESTRICTED_ACCESS_SD
// The following function initializes the supplied security descriptor
// with a DACL that grants the Authenticated Users group GENERIC_READ,
// GENERIC_WRITE, and GENERIC_EXECUTE access.
// 
// The function returns NULL if any of the access control APIs fail.
// Otherwise, it returns a PVOID pointer that should be freed by calling
// FreeRestrictedSD() after the security descriptor has been used to
// create the object.

static PVOID BuildRestrictedSD( PSECURITY_DESCRIPTOR pSD ) noexcept
{
	BOOL   bResult = FALSE;

	SID_IDENTIFIER_AUTHORITY siaNT = SECURITY_WORLD_SID_AUTHORITY/*SECURITY_NT_AUTHORITY*/;

	PSID pAuthenticatedUsersSID = nullptr;
	PACL pDACL = nullptr;

	__try
	{
		// initialize the security descriptor
		if ( !InitializeSecurityDescriptor( pSD, SECURITY_DESCRIPTOR_REVISION ) )
		{
			//			printf("InitializeSecurityDescriptor() failed with error %d\n", GetLastError());
			__leave;
		}

		// obtain a sid for the Authenticated Users Group
		if ( !AllocateAndInitializeSid( &siaNT, 1,
			SECURITY_WORLD_RID/*SECURITY_AUTHENTICATED_USER_RID*/, 0, 0, 0, 0, 0, 0, 0,
			&pAuthenticatedUsersSID ) )
		{
			//				printf("AllocateAndInitializeSid() failed with error %d\n", GetLastError());
			__leave;
		}

		// NOTE:
		// 
		// The Authenticated Users group includes all user accounts that
		// have been successfully authenticated by the system. If access
		// must be restricted to a specific user or group other than 
		// Authenticated Users, the SID can be constructed using the
		// LookupAccountSid() API based on a user or group name.

		// calculate the DACL length
		const DWORD dwAclLength = sizeof( ACL )
			// add space for Authenticated Users group ACE
			+ sizeof( ACCESS_ALLOWED_ACE ) - sizeof( DWORD )
			+ GetLengthSid( pAuthenticatedUsersSID );

		// allocate memory for the DACL
		pDACL = (PACL) malloc( dwAclLength );
		if ( !pDACL )
		{
			//			printf("HeapAlloc() failed with error %d\n", GetLastError());
			__leave;
		}

		// initialize the DACL
		if ( !InitializeAcl( pDACL, dwAclLength, ACL_REVISION ) )
		{
			//			printf("InitializeAcl() failed with error %d\n", GetLastError());
			__leave;
		}

		// add the Authenticated Users group ACE to the DACL with just the event access rights needed
		if ( !AddAccessAllowedAce( pDACL, ACL_REVISION, EVENT_ALL_ACCESS, pAuthenticatedUsersSID ) )
		{
			//			printf("AddAccessAllowedAce() failed with error %d\n", GetLastError());
			__leave;
		}

		// set the DACL in the security descriptor
		if ( !SetSecurityDescriptorDacl( pSD, TRUE, pDACL, FALSE ) )
		{
			//			printf("SetSecurityDescriptorDacl() failed with error %d\n", GetLastError());
			__leave;
		}

		bResult = TRUE;

	}
	__finally
	{
		if ( pAuthenticatedUsersSID )
		{
			FreeSid( pAuthenticatedUsersSID );
		}
	}

	if ( !bResult )
	{
		free( pDACL );
		pDACL = nullptr;
	}

	return pDACL;
}

// The following function frees memory allocated in the
// BuildRestrictedSD() function
static VOID FreeRestrictedSD( PVOID ptr ) noexcept
{
	free( ptr );
}
#endif

/* The GUID used to name the event used to signal other instances of changing settings */
#define EVENT_GUID _T("CA56EB1D-CE28-487d-8572-E071B8507203")

/* This GUID names the event to signal other instances to close down - as part of the uninstallation */
#define UNINST_GUID _T("{4AC75521-5B5A-4c49-B9A1-46EF08628F12}")

/* This names the event to cause monitor processes to restart themselves */
#define RESTART_GUID _T("{438A210C-8FED-447f-8265-89CFD39DFA7F}")

void InitEvents()
{
	{
		SECURITY_DESCRIPTOR sd;

#ifdef RESTRICTED_ACCESS_SD
		// build a restricted security descriptor
		PVOID ptr = BuildRestrictedSD( &sd );
		if ( ptr == nullptr )
		{
			/* We're probably running on Win9x, so this is expected to fail - do the old thing */
			if ( InitializeSecurityDescriptor( &sd, SECURITY_DESCRIPTOR_REVISION ) )
			{
				// Add a NULL DACL to the security descriptor.
				if ( SetSecurityDescriptorDacl( &sd, TRUE, nullptr, FALSE ) )
				{
				}
			}
		}
#endif
		SECURITY_ATTRIBUTES sa;
		// create a mutex using the security descriptor
		sa.nLength = sizeof( sa );
		sa.lpSecurityDescriptor = &sd;
		sa.bInheritHandle = FALSE;

#ifndef RESTRICTED_ACCESS_SD
		if ( InitializeSecurityDescriptor( &sd, SECURITY_DESCRIPTOR_REVISION ) )
		{
			// Add a NULL DACL to the security descriptor.
			if ( SetSecurityDescriptorDacl( &sd, TRUE, NULL, FALSE ) )
			{
#endif

				/* This event is used to signal other instances to refresh their settings */
				g_hEvents[EVT_REFRESH] = CreateEvent( &sa, true, false, _T( "Global\\" ) EVENT_GUID );

				/* In non-terminal server environments the above fails since
				* "Global" events don't exist. So to keep the other code
				* consistent, create the event locally.
				*/
				if ( g_hEvents[EVT_REFRESH] == NULL )
				{
					g_hEvents[EVT_REFRESH] = CreateEvent( &sa, true, false, EVENT_GUID );
				}
				_ASSERT( g_hEvents[EVT_REFRESH] != NULL );

				/* This event signals closedown of this process */
				g_hEvents[EVT_EXITTHREAD] = CreateEvent( NULL, true, false, NULL );
				_ASSERT( g_hEvents[EVT_EXITTHREAD] != NULL );

				g_hEvents[EVT_EXITINSTANCE] = CreateEvent( &sa, true, false, _T( "Global\\" ) UNINST_GUID );
				if ( g_hEvents[EVT_EXITINSTANCE] == NULL )
				{
					g_hEvents[EVT_EXITINSTANCE] = CreateEvent( &sa, true, false, UNINST_GUID );
				}
				_ASSERT( g_hEvents[EVT_EXITINSTANCE] != NULL );

				g_hEvents[EVT_RESTARTMONITORS] = CreateEvent( &sa, true, false, _T( "Global\\" ) RESTART_GUID );
				if ( g_hEvents[EVT_RESTARTMONITORS] == NULL )
				{
					g_hEvents[EVT_RESTARTMONITORS] = CreateEvent( &sa, true, false, RESTART_GUID );
				}
				_ASSERT( g_hEvents[EVT_RESTARTMONITORS] != NULL );

#ifdef RESTRICTED_ACCESS_SD
				// free the memory allocated by BuildRestrictedSD
				FreeRestrictedSD( ptr );
#else
			}
		}
#endif
	}
}
