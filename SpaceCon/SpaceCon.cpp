// SpaceCon.cpp : Defines the entry point for the application.
//

#include "stdafx.h"

#include <atlbase.h>
#include <atltime.h>

#include <Shellapi.h>
#include <commctrl.h>
#include <Shlwapi.h>
#pragma comment( lib, "Shlwapi.lib" )
#include <strsafe.h>
#include <VersionHelpers.h>
#include <process.h>
#include <vector>
#include <optional>
#include <filesystem>

using std::vector;
using std::optional;
using std::filesystem::path;

#include "RegDataV3.h"
#include "RegKeyRegistryFuncs.h"

#include "SpaceCon.h"

#include "AboutDlg.h"
#include "MiscFunctions.h"

#include "CommonToBoth.h"


// Global Variables:
static HINSTANCE g_hInstance;			// current instance
static HINSTANCE g_hResInst;			// Resource DLL instance
static TCHAR szAppName[100];			// The title bar text
static LPCTSTR szWindowClass = _T("JDDESIGN_SPACECON");

/* These are the registry key/value for the XP facility */
#define EXPLORER_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer")
#define EXPLORER_VAL _T("NoLowDiskSpaceChecks")

//static BOOL bREGISTERED;
static optional<CMyRegData> g_RegData;

#define AMEGABYTE	(1024*1024)

/* The OS version information */
//static OSVERSIONINFO g_vi;
#if 0
/* The following are the Win98 registry settings to enable/disable its inbuilt low-disk space notifications */
#define REGSTR_PATH_FILESYSTEM _T("System\\CurrentControlSet\\Control\\FileSystem")
#define REGSTR_VAL_DRIVE_LDS_BDCAST_DISABLE _T("DisableLowDiskSpaceBroadcast")

static DWORD GetWin98LDSFlags( void )
{
	DWORD dwFlags = 0;
	LONG res;
	CRegKey rk;

	res = rk.Open( HKEY_LOCAL_MACHINE, REGSTR_PATH_FILESYSTEM, KEY_READ );

	if ( res == ERROR_SUCCESS )
	{
		/* Read the value */
		res = rk.QueryDWORDValue( REGSTR_VAL_DRIVE_LDS_BDCAST_DISABLE, dwFlags );
	}

	return dwFlags;
}

static void SetWin98LDSFlags( DWORD dwFlags )
{
	LONG res;
	CRegKey rk;

	res = rk.Open( HKEY_LOCAL_MACHINE, REGSTR_PATH_FILESYSTEM, KEY_SET_VALUE );

	if ( res == ERROR_SUCCESS )
	{
		/* Read the value */
		res = rk.SetDWORDValue( REGSTR_VAL_DRIVE_LDS_BDCAST_DISABLE, dwFlags );
	}
}
#endif

typedef struct tagColStruct
{
	int StringID;		// The resource string ID for the list caption
	BYTE WidthInChars;	// Default width - value is read from resource if it's there
	int Align;			// Column alignment fmt
} COLSTRUCT;

/* This structure contains the column header title texts, and the default CHARACTER widths */
static COLSTRUCT g_columnFmts[] =
{
	IDS_DRIVE, 15, LVCFMT_LEFT,
	IDS_CAPACITY, 9, LVCFMT_RIGHT,
	IDS_FREE_SPACE, 10, LVCFMT_RIGHT,
// NT Specific	_T("User Free"), 9,
	IDS_ALARM_AT, 9, LVCFMT_RIGHT,
	IDS_RECOMMEND, 20, LVCFMT_LEFT
//	_T("Type"), 7
};

enum Cols { COL_DRIVE, COL_SIZE, COL_TOT_FREE, /*COL_USER_FREE,*/ COL_NOTIFY, COL_RECOMENDATION/*COL_TYPE*/ };


/* This is the data that's stored per-item (drive) in the list control.
 * It's used to pass the data to the modify (size) dialog box.
 */
class CModDlgParams
{
public:
	// These items are used solely to convey display information to the modify dialog

	TCHAR szVolName[MAX_PATH+1];	// The drive name
	ULONGLONG DriveSize;	// Total Size
	ULONGLONG FreeSpace;	// The free space on this disk drive
	ULONGLONG AlarmAtMB;	// The threshold point

	/* This is needed to associate the list item with the physical disk drive number (for the global setting store g_DriveConfig) */
	int DriveNum;			// The drive number
	TCHAR szDrive[_MAX_DRIVE+1];	// The drive letter string "C:\" (needed to do the list refresh)
	CModDlgParams() noexcept
	{
		szVolName[0] = _T('\0');
		DriveSize = 0;
		FreeSpace = 0;
		AlarmAtMB = 0;
		DriveNum = -1;
		szDrive[0] = _T('\0');
	}
};

static void UpdateDriveInformation( HWND hList, int Item, WORD DriveNum, LPCTSTR pDrive, bool bUpdate )
{
	CModDlgParams * pItemData;
	LVITEM lvi;

	/* Only insert the item if it's the initialisation time, refreshes overwrite */
	if ( !bUpdate )
	{
		/* Allocate a new per-item data */
		pItemData = new CModDlgParams();

		pItemData->DriveNum = DriveNum;
		lstrcpyn( pItemData->szDrive, pDrive, _countof( pItemData->szDrive ) );

		/* Get the cosmetic drive name */
		SHFILEINFO sfi;
		if ( SHGetFileInfo( pItemData->szDrive, FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof( sfi ), SHGFI_DISPLAYNAME ) )
		{
			StringCchCopy( pItemData->szVolName, _countof( pItemData->szVolName ), sfi.szDisplayName );
		}
#if 0
//Old version - not necessary, the above does it better - handles empty volume labels the same as Explorer does.
		else
		{
			DWORD dwMaxLen, dwFSFlags;

			GetVolumeInformation( pItemData->szDrive, pItemData->szVolName, DIM( pItemData->szVolName ) - sizeof(" (C:)"),
					NULL, &dwMaxLen, &dwFSFlags, NULL/*szFSType*/, 0/*sizeof( szFSType )*/ );

			PathMakePretty( pItemData->szVolName );

			/* Append formatted drive letter to the volume name */
			const int curlen = lstrlen(pItemData->szVolName);
			StringCchPrintf( &pItemData->szVolName[curlen], DIM(pItemData->szVolName)-curlen,
				_T(" (%c:)"), /*(BYTE) CharUpper( MAKEINTRESOURCE( */pItemData->szDrive[0] /*) ) */);
		}
#endif
		lvi.mask = LVIF_TEXT | LVIF_PARAM;
		lvi.iItem = Item;
		lvi.iSubItem = 0;
		lvi.pszText = pItemData->szVolName;

		/* Although all the data structure isn't populated, because the state
		 * change notification happens as a result of subsequent operations here,
		 * I need to set up the drive number ready to handle this.
		 */
		lvi.lParam = (LPARAM) (DWORD_PTR) pItemData;

		/* Can't set the check box when inserting */
		ListView_InsertItem( hList, &lvi );

		/* Set the check box to indicate the current enabled/disabled state. */
		ListView_SetCheckState( hList, Item, g_DriveConfig[ pItemData->DriveNum ].bCheckMe );
	}
	else
	{
		lvi.mask = LVIF_PARAM;
		lvi.iItem = Item;
		ListView_GetItem( hList, &lvi );
        pItemData = (CModDlgParams *) lvi.lParam;
	}

	ULARGE_INTEGER CallerFreeBytes, TotalBytes, TotalFreeBytes;
	WCHAR szBufferW[20];

	/* Hard to believe, but the stupid ASCII version of
	 * StrFormatByteSize, only takes a DWORD size!
	 */
	if ( GetDiskFreeSpaceEx( pItemData->szDrive, &CallerFreeBytes, &TotalBytes, &TotalFreeBytes ) )
	{
		StrFormatByteSizeW( TotalBytes.QuadPart, szBufferW, _countof( szBufferW ) );
		{
		CW2T pT( szBufferW );
		lvi.pszText = pT;
		lvi.iSubItem = COL_SIZE;
		::SendMessage( hList, LVM_SETITEMTEXT, Item, (LPARAM) &lvi );
		}

		StrFormatByteSizeW( TotalFreeBytes.QuadPart, szBufferW, _countof( szBufferW ) );
		{
		CW2T pT( szBufferW );
		lvi.pszText = pT;
		lvi.iSubItem = COL_TOT_FREE;
		::SendMessage( hList, LVM_SETITEMTEXT, Item, (LPARAM) &lvi );
		}

		pItemData->DriveSize = TotalBytes.QuadPart;
        pItemData->FreeSpace = TotalFreeBytes.QuadPart;
	}
	

	{
		// Detect the uninitialised state - AlarmAt == 0, and replace with something more reasonable
		if ( g_DriveConfig[pItemData->DriveNum].AlarmAt == 0 )
		{
			// Use 15% figure initially
			g_DriveConfig[pItemData->DriveNum].AlarmAt = TotalBytes.QuadPart * 15 / 100;
		}

		/* Calculate in some meaningful way, the actual alarm size figure for this drive */
		StrFormatByteSizeW( g_DriveConfig[pItemData->DriveNum].AlarmAt, szBufferW, _countof( szBufferW ) );
		CW2T pT( szBufferW );
		lvi.pszText = pT;
		lvi.iSubItem = COL_NOTIFY;
		::SendMessage( hList, LVM_SETITEMTEXT, Item, (LPARAM) &lvi );
//		ListView_SetItemText( hList, Item, COL_NOTIFY, szBufferW );

		pItemData->AlarmAtMB = g_DriveConfig[ pItemData->DriveNum ].AlarmAt;

		/* Try to make some recommendation based on the free space & current setting */
		int MsgID;

		if ( TotalFreeBytes.QuadPart < 1ULL * AMEGABYTE )
		{
			/* The total free space on the drive is very limited - advise freeing some space before it's too late */
			MsgID = IDS_RECOMEND_VERY_LOW;
		}
		else
		{
			/* Have we got less than the user's alarm setting? */
			if ( g_DriveConfig[ pItemData->DriveNum ].AlarmAt >= TotalFreeBytes.QuadPart )
			{
				/* Yes, we're at the alarm point */
				/* Have we got sufficient space to defragment the drive? */
				if ( TotalFreeBytes.QuadPart < ( 15 * pItemData->DriveSize ) / 100)
				{
					MsgID = IDS_RECOMMEND_FREE_SPACE;
				}
				else
				{
					/* We've already exceeded the alarm point - better alter it to prevent repeated alarms */
					MsgID = IDS_RECOMEND_BELOW_THRESHOLD;
				}
			}
			else
			{
				if ( ( TotalFreeBytes.QuadPart - g_DriveConfig[ pItemData->DriveNum ].AlarmAt ) < 2ULL * AMEGABYTE )
				{
					/* We're pretty close to the alarm point - watch out! */
					MsgID = IDS_RECOMEND_NEAR_THRESHOLD;
				}
				else
				{
					MsgID = 0;
				}
			}
		}

		lvi.iSubItem = COL_RECOMENDATION;

		if ( MsgID != 0 )
		{
			CString str;
			str.LoadString( g_hInstance, MsgID );
			lvi.pszText = const_cast<LPTSTR>(static_cast<LPCTSTR>(str));

			SendMessage( hList, LVM_SETITEMTEXT, Item, (LPARAM) &lvi );
//			ListView_SetItemText( hList, Item, COL_RECOMENDATION, szMsg );
		}
		else
		{
			lvi.pszText = nullptr;
			SendMessage( hList, LVM_SETITEMTEXT, Item, (LPARAM) &lvi );
//			ListView_SetItemText( hList, Item, COL_RECOMENDATION, NULL );
		}
	}
}

LRESULT CALLBACK ModifyDlg(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	static CModDlgParams * pP;

	switch (message)
	{
	case WM_INITDIALOG:
		{
			pP = (CModDlgParams *) lParam;
			SetDlgItemText( hDlg, IDC_DRIVE, pP->szVolName );

			WCHAR szBufferW[20];

			StrFormatByteSizeW( pP->DriveSize, szBufferW, _countof( szBufferW ) );
			{
			CW2CT pT( szBufferW );
			SetDlgItemText( hDlg, IDC_DRIVE_SIZE, pT );
			}
			
			StrFormatByteSizeW( pP->FreeSpace, szBufferW, _countof( szBufferW ) );
			{
			CW2CT pT( szBufferW );
			SetDlgItemText( hDlg, IDC_DRIVE_FREE, pT );
			}

			/* Display/edit the threshold value in MB */
			SetDlgItemInt( hDlg, IDC_ALARMAT, static_cast<UINT>(pP->AlarmAtMB/AMEGABYTE), false );
		}
		return TRUE;

	case WM_COMMAND:
		switch( LOWORD(wParam ) )
		{
		case IDOK:
			pP->AlarmAtMB = GetDlgItemInt( hDlg, IDC_ALARMAT, NULL, false );
			pP->AlarmAtMB *= AMEGABYTE;
			[[fallthrough]];	// Intentional!

		case IDCANCEL:
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;

		case IDC_PRESET:
			/* Calculate 15% of the capacity */
			pP->AlarmAtMB = ( pP->DriveSize * 15 ) / 100;
			SetDlgItemInt( hDlg, IDC_ALARMAT, static_cast<UINT>(pP->AlarmAtMB/AMEGABYTE), false );
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static bool IsXP()
{
	return ::IsWindowsXPSP3OrGreater();
}
#if 0
static bool IsWin98()
{
	return ( g_vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS ) && ( g_vi.dwMajorVersion >= 4 ) && ( g_vi.dwMinorVersion >= 10 );
}
#endif
LRESULT CALLBACK ConfigDlg(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	static bool g_bModified = false;

	switch (message)
	{
	case WM_INITDIALOG:
		{
			const HWND hList = GetDlgItem( hDlg, IDC_LIST );

			/* Give the list control the full row select & checkboxes for each item */
			ListView_SetExtendedListViewStyleEx( hList, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES,
						LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES );

			/* Initialise the columns of the list control */
			{
				int CharWidth;

				/* Get the pixel width of a character in this dialog box font */
				{
					TEXTMETRIC tm;
					HDC hDC = GetDC( hList );

					GetTextMetrics( hDC, &tm );

					CharWidth = tm.tmAveCharWidth;

					ReleaseDC( hDlg, hDC );
				}

				/* Load the custom resource containing the column sizes.
				 * Only need to do this once, hence the static here.
				 */
				static HRSRC hRsrc;
				
				if ( hRsrc == NULL )
				{
					hRsrc = FindResource( g_hResInst, MAKEINTRESOURCE( IDR_VALS ), _T("RT_RCDATA") );
					if ( hRsrc != NULL )
					{
						HGLOBAL hCols = LoadResource( g_hResInst, hRsrc );

						if ( hCols != NULL )
						{
							const LPCBYTE pCols = (LPCBYTE) LockResource( hCols );

							DWORD csResSize = SizeofResource( g_hResInst, hRsrc );

							// For safety, do the least we have
							csResSize = min( csResSize, _countof(g_columnFmts ) );

							/* Copy the bytes over the embedded column structure */
							for ( UINT ColNo = 0; ColNo < csResSize; ColNo++ )
							{
								g_columnFmts[ColNo].WidthInChars = pCols[ColNo];
							}

							UnlockResource( hCols );
							FreeResource( hCols );
						}
					}
				}

				for ( UINT ColNo = 0; ColNo < _countof(g_columnFmts); ColNo++ )
				{
					LVCOLUMN lvc;
					lvc.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;
					lvc.fmt = g_columnFmts[ColNo].Align;

					CString sCaption;
					sCaption.LoadString( g_hResInst, g_columnFmts[ColNo].StringID );
					lvc.pszText = const_cast<LPTSTR>(static_cast<LPCTSTR>(sCaption));

					lvc.cx = g_columnFmts[ColNo].WidthInChars * CharWidth;

					ListView_InsertColumn( hList, ColNo, &lvc );
				}

				/* Populate the list */
				{
					const auto ReqdBufferSize = GetLogicalDriveStrings( 0, nullptr );
					vector<TCHAR> szDriveStrings( ReqdBufferSize );
					GetLogicalDriveStrings( ReqdBufferSize, szDriveStrings.data() );

					const DWORD dwDrives = GetLogicalDrives();
					int Item;
					WORD dNum;

					/* Loop for all disk drives on the system (A...Z) */
					LPCTSTR pDrive;

					for ( dNum = 0, Item = 0, pDrive = szDriveStrings.data(); dNum < 26; dNum++ )
					{
						/* If the bit is set for the drive */
						if ( dwDrives & (1 << dNum ) )
						{
							const UINT drivetype = GetDriveType( pDrive );

							/* Check hard disks and removable ones like ZIP drives (unfortunately, this includes floppys too) */
							if ( ( DRIVE_FIXED == drivetype ) /*|| ( DRIVE_REMOVABLE == drivetype )*/ )
							{
								UpdateDriveInformation( hList, Item, dNum, pDrive, false );

								Item++;
							}

							/* Next drive letter */
							pDrive = &pDrive[ lstrlen( pDrive ) + 1 ];
						}
					}

					// Calling UpdateDriveInformation above gives rise to the
					// change event on the check boxes, which sets this flag,
					// so clear it here
					g_bModified = false;
				}

				/* Make the last column use all the space left */
				ListView_SetColumnWidth( hList, COL_RECOMENDATION, LVSCW_AUTOSIZE_USEHEADER );

				/* Select the first item in the list */
				ListView_SetItemState( hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED );

				/* Get the current OS setting reflected in the check box */
				{
					DWORD Value = 0;

					if ( IsXP() )
					{
						CRegKey rk;

						if ( ERROR_SUCCESS == rk.Open( HKEY_LOCAL_MACHINE, EXPLORER_KEY, KEY_READ ) )
						{
							if ( ERROR_SUCCESS == rk.QueryDWORDValue( EXPLORER_VAL, Value ) )
							{
								;
							}
							else
							{
								;
							}
						}
					}
					//else
					//if ( IsWin98() )
					//{
					//	Value = GetWin98LDSFlags();
					//}

					CheckDlgButton( hDlg, IDC_DISABLE_OS, Value != 0 ? BST_CHECKED: BST_UNCHECKED );
				}
			}

			/* Enable the OS feature if it's not an OS with the feature */
			EnableWindow( GetDlgItem( hDlg, IDC_DISABLE_OS), IsXP() /*|| IsWin98()*/ );
		}
		return TRUE;

	case WM_NOTIFY:
		{
			LPNMHDR phdr = (LPNMHDR) lParam;
			switch( phdr->code )
			{
			case LVN_ITEMCHANGED:
				{
					LPNMLISTVIEW plv = (LPNMLISTVIEW) lParam;
					if ( plv->uChanged & LVIF_STATE )
					{
						if ( plv->uOldState && ( plv->uNewState & LVIS_STATEIMAGEMASK ) )
						{
							const BOOL bState = ListView_GetCheckState( phdr->hwndFrom, plv->iItem );
							CModDlgParams * mp = reinterpret_cast<CModDlgParams *>( plv->lParam );
							g_DriveConfig[ mp->DriveNum  ].bCheckMe = bState ? true: false;

							/* Set the modified flag */
							g_bModified = true;
						}
					}
				}
				break;

			case LVN_DELETEALLITEMS:
				{
					LPNMLISTVIEW pnmv = (LPNMLISTVIEW) lParam;

					const int NumItems = ListView_GetItemCount( pnmv->hdr.hwndFrom );

					/* Delete the per-item data */
					for ( int indx = 0; indx < NumItems; indx++ )
					{
						LVITEM lvi;
						lvi.mask = LVIF_PARAM;
						lvi.iItem = indx;
						ListView_GetItem( pnmv->hdr.hwndFrom, &lvi );
						CModDlgParams * mp = reinterpret_cast<CModDlgParams *>(lvi.lParam);
						delete mp;
					}

					/* We've processed this message and don't need the individual delete messages */
					SetWindowLong( hDlg, DWL_MSGRESULT, TRUE );
					return TRUE;
				}
				break;

			case NM_DBLCLK:
				/* Do the same as single click and the default operation - Modify */
				PostMessage( hDlg, WM_COMMAND, IDOK, 0 );
				break;

			default:
				break;
			}
		}
		break;

	case WM_COMMAND:
		switch( LOWORD( wParam ) )
		{
		// The Modify button
		case IDOK:
			{
				const HWND hList = GetDlgItem( hDlg, IDC_LIST );

				const int SelItem = ListView_GetNextItem( hList, -1, LVNI_SELECTED );
                if ( SelItem != -1 )
                {
					LVITEM lvi;
					lvi.mask = LVIF_PARAM;
					lvi.iItem = SelItem;
					ListView_GetItem( hList, &lvi );
					CModDlgParams * mp = reinterpret_cast<CModDlgParams *>(lvi.lParam);

                    if ( IDOK == DialogBoxParam( g_hResInst, MAKEINTRESOURCE( IDD_MOD_DLG ), hDlg, (DLGPROC)ModifyDlg, (LPARAM) mp ) )
					{
						/* Modified value returned - copy alarm value to global settings */
						g_DriveConfig[ mp->DriveNum ].AlarmAt = mp->AlarmAtMB;

						/* Update list display */
						UpdateDriveInformation( hList, SelItem, 0, NULL, true );

						/* Set the modified flag */
						g_bModified = true;
					}
				}
			}
			break;

		case IDC_DISABLE_OS:
			if ( HIWORD( wParam ) == BN_CLICKED )
			{
				/* Set the modified flag */
				g_bModified = true;
			}
			break;

		case IDC_SAVE:
			{
				/* Have we made any changes? */
				if ( g_bModified )
				{
					/* Write our settings to the registry */
					HKEY hKey;

					LONG RegRes = RegCreateKeyEx( HKEY_LOCAL_MACHINE, szRegistryKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL );

					if ( ERROR_SUCCESS == RegRes )
					{
						/* Store the global structure */
						RegRes = RegSetValueEx( hKey, SETTINGS, 0, REG_BINARY, (LPBYTE) &g_DriveConfig, sizeof( g_DriveConfig ) );

						if ( ERROR_SUCCESS == RegRes )
						{
						}
						else
						{
							ResMessageBox( hDlg, IDS_FAILED_WRITE_REG/*_T("Failed to write the values to the registry")*/, szAppName, MB_OK | MB_ICONINFORMATION );
						}

						RegCloseKey( hKey );
					}
					else
					{
						/* Failed to open/create the registry key. Probably don't have permission to do it */
						if ( RegRes == ERROR_ACCESS_DENIED )
						{
							/* tell the user they don't have suitable permission to change the settings */
							ResMessageBox( hDlg, IDS_YOU_CANT_CHANGE_SETTINGS, szAppName, MB_OK | MB_ICONINFORMATION );
						}
						else
						{
							/* Report the system error we get back */
							LPVOID lpMsgBuf;
							if (FormatMessage( 
								FORMAT_MESSAGE_ALLOCATE_BUFFER | 
								FORMAT_MESSAGE_FROM_SYSTEM | 
								FORMAT_MESSAGE_IGNORE_INSERTS,
								NULL,
								RegRes,
								MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
								(LPTSTR) &lpMsgBuf,
								0,
								NULL ))
							{
								// Display the string.
								MessageBox( hDlg, (LPCTSTR)lpMsgBuf, szAppName, MB_OK | MB_ICONINFORMATION );

								// Free the buffer.
								LocalFree( lpMsgBuf );
							}
							else
							{
								// Handle the error.
			//					return;
							}
						}
					}

					/* Switch on/off the Windows XP settings */
					const bool bEnableOSSettings = IsDlgButtonChecked( hDlg, IDC_DISABLE_OS ) == BST_CHECKED ? false: true;

					/* Is it WinXP or compatible? */
					if ( IsXP() )
					{
						const LONG RRes = RegCreateKeyEx( HKEY_LOCAL_MACHINE, EXPLORER_KEY, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL );
						if ( ERROR_SUCCESS == RRes )
						{
							const DWORD Value = bEnableOSSettings ? 0: 1;

							const LONG RegRes1 = RegSetValueEx( hKey, EXPLORER_VAL, 0, REG_DWORD, (LPBYTE) &Value, sizeof( Value ) );

							if ( ERROR_SUCCESS == RegRes1 )
							{
							}
							else
							{
								LPVOID lpMsgBuf;
								if (FormatMessage( 
									FORMAT_MESSAGE_ALLOCATE_BUFFER | 
									FORMAT_MESSAGE_FROM_SYSTEM | 
									FORMAT_MESSAGE_IGNORE_INSERTS,
									NULL,
									RegRes1,
									MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
									(LPTSTR) &lpMsgBuf,
									0,
									NULL ))
								{
									// Display the string.
									MessageBox( hDlg, (LPCTSTR)lpMsgBuf, szAppName, MB_OK | MB_ICONINFORMATION );

									// Free the buffer.
									LocalFree( lpMsgBuf );
								}
								else
								{
								}
							}

							RegCloseKey( hKey );
						}
						else
						{
							/* Only report this error if we've not already had another similar one */
							if ( RegRes == ERROR_SUCCESS )
							{
								RegRes = RRes;

								if ( RegRes == ERROR_ACCESS_DENIED )
								{
									/* tell the user they don't have suitable permission to change this setting */
									ResMessageBox( hDlg, IDS_YOU_CANT_CHANGE_OS_SETTING, szAppName, MB_OK | MB_ICONINFORMATION );
								}
								else
								{
									/* Report the system error we get back */
									LPVOID lpMsgBuf;
									if (FormatMessage( 
										FORMAT_MESSAGE_ALLOCATE_BUFFER | 
										FORMAT_MESSAGE_FROM_SYSTEM | 
										FORMAT_MESSAGE_IGNORE_INSERTS,
										NULL,
										RegRes,
										MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
										(LPTSTR) &lpMsgBuf,
										0,
										NULL ))
									{
										// Display the string.
										MessageBox( hDlg, (LPCTSTR)lpMsgBuf, szAppName, MB_OK | MB_ICONINFORMATION );

										// Free the buffer.
										LocalFree( lpMsgBuf );
									}
									else
									{
										// Handle the error.
					//					return;
									}
								}
							}
						}
					}
					//else
					///* Is it Win98? */
					//if ( IsWin98() )
					//{
					//	const DWORD dwVal = bEnableOSSettings ? 0: (DWORD) -1;

					//	SetWin98LDSFlags( dwVal );
					//}

					/* Clear the modified flag */
					g_bModified = false;
				}

				/* Notify any other running instances that the settings have changed */
				Sleep(0); // See Q173260
				if ( !PulseEvent( g_hEvents[EVT_REFRESH] ) )
				{
					/* Failed! Debug point only */
					g_hEvents[EVT_REFRESH] = g_hEvents[EVT_REFRESH];
				}
			}
			break;

		case IDCANCEL:
			if ( !g_bModified || ( ResMessageBox( hDlg, IDS_UNSAVED_PROMPT, szAppName,
													MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 ) == IDYES ) )
			{
				EndDialog( hDlg, LOWORD( wParam ) );
				return TRUE;
			}
			else
			{
				break;
			}

		case IDC_ABOUT:
			/* Invoke the about code from the parent window */
			{
				HWND hWnd = GetParent( hDlg );

				/* Invoke the command, but have it use our window as the parent */
				PostMessage( hWnd, WM_COMMAND, IDM_ABOUT, (LPARAM) hDlg );
			}
			return TRUE;

		case IDC_CONFIG_HELP:
			{
				path HelpPath{ GetModuleFilePath( g_hInstance ) };
				// The help file is in a sub-dir
				HelpPath.replace_filename( _T("docs\\SpacePatrol.html") );

				ShellExecute( hDlg, NULL, HelpPath.c_str(), NULL, NULL, SW_NORMAL);
			}
			break;
		}
		break;

	case WM_DESTROY:
		WinHelp( hDlg, NULL, HELP_QUIT, 0 );
		break;
	}
	return FALSE;
}

//  FUNCTION: WndProc(HWND, unsigned, WORD, LONG)
//
//  PURPOSE:  Processes messages for the main window.
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) 
	{
	case WM_COMMAND:
		{
		int wmId, wmEvent;
		wmId    = LOWORD(wParam); 
		wmEvent = HIWORD(wParam);
		static bool bInHere = false;
		// Parse the menu selections:
		switch (wmId)
		{
		case ID_CONFIG:
			if ( !bInHere )
			{
				bInHere = true;
				DialogBox(g_hResInst, MAKEINTRESOURCE( IDD_CONFIG_DLG ), hWnd, (DLGPROC)ConfigDlg);
#ifndef _DEBUG
				PostMessage( hWnd, WM_CLOSE, 0, 0 );
#endif
				bInHere = false;
			}
			else
			{
//				::BringWindowToTop( hWnd );
			}
			break;

		case IDM_ABOUT:
			{
				HWND hParent;

				/* If the message is sent from the config dialog, lParam is the window handle that should be the parent */
				if ( lParam == NULL )
				{
					/* No explicit parent - so just use this window */
					hParent = hWnd;
				}
				else
				{
					hParent = (HWND) lParam;
				}

				AboutHandler( hParent,
								g_RegData,
								g_hResInst,
								szAppName,
								L"Thanks for using Space Patrol",
								szRegistryKey,
								ProductCode::SpacePatrol,
								IDS_CLOSE_FOR_REG );
			}
			break;

		case IDM_EXIT:
			DestroyWindow(hWnd);
			break;

		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
		}
		}
		break;

	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc;
			hdc = BeginPaint(hWnd, &ps);
			EndPaint(hWnd, &ps);
		}
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
static ATOM MyRegisterClass(HINSTANCE hInstance) noexcept
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX); 

	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= (WNDPROC)WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= LoadIcon(hInstance, (LPCTSTR)IDI_SPACECON);
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= MAKEINTRESOURCE(IDC_SPACECON);
	wcex.lpszClassName	= szWindowClass;
	wcex.hIconSm		= (HICON) LoadImage( wcex.hInstance, MAKEINTRESOURCE( IDI_SPACECON ), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR );

	return RegisterClassEx(&wcex);
}

static BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	g_hResInst = g_hInstance = hInstance; // Store instance handle in our global variable

	/* Get the OS information for some decisions we need */
	//g_vi.dwOSVersionInfoSize = sizeof( g_vi );
	//GetVersionEx( &g_vi );

	InitEvents();

	/* Process command line switches */
	for ( int indx = 1; indx < __argc; indx++ )
	{
		/* Is it a switch character? */
		const TCHAR chSw = __targv[indx][0];

		if ( ( chSw == _T('/') ) || ( chSw == _T('-') ) )
		{
			switch ( reinterpret_cast<ULONG_PTR>( CharUpper( MAKEINTRESOURCE( __targv[indx][1] ) ) ) )
			{
			case 'U':	// Uninstall
#ifdef _DEBUG
				MessageBox( GetForegroundWindow(), _T("Uninstall"), szAppName, MB_OK );
#endif
				/* Notify any other monitoring instances that we're being uninstalled */
				Sleep(0); // See Q173260
				PulseEvent( g_hEvents[EVT_EXITINSTANCE] );
				return FALSE;
			}
		}
	}

	g_RegData = GetMyRegistrationFromTheRegistry( szRegistryKey );

	/* Load the settings from the registry */
	LoadGlobalSettingsFromReg();

	HWND hWnd = CreateWindow(szWindowClass, szAppName, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 350, 100, NULL, NULL, hInstance, NULL);

	if (!hWnd)
	{
		return FALSE;
	}

#ifdef _DEBUG
	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);
#else
	nCmdShow;	// Prevent compiler warning
#endif

	/* Display the main UI dialog */
	PostMessage( hWnd, WM_COMMAND, ID_CONFIG, 0 );

	return TRUE;
}

int APIENTRY _tWinMain(HINSTANCE hInstance,
                     HINSTANCE /*hPrevInstance*/,
                     LPTSTR    /*lpCmdLine*/,
                     int       nCmdShow)
{
	/* Debug version memory leak checking */
	_CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );

	MSG msg;
	HACCEL hAccelTable;

	// Initialize global strings
	LoadString(hInstance, IDS_APP_TITLE, szAppName, std::size( szAppName ) );
//	LoadString(hInstance, IDC_SPACECON, szWindowClass, _countof( szWindowClass ) );
	MyRegisterClass(hInstance);

	// Perform application initialization:
	if (!InitInstance (hInstance, nCmdShow)) 
	{
		return FALSE;
	}

	hAccelTable = LoadAccelerators(hInstance, (LPCTSTR)IDC_SPACECON);

	// Main message loop:
	while (GetMessage(&msg, NULL, 0, 0)) 
	{
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) 
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return (int) msg.wParam;
}
