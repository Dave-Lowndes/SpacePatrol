// SPMonitor.cpp : Defines the entry point for the application.
//

#include "stdafx.h"
#include <atlbase.h>
#include <atlstr.h>
#include <Shellapi.h>
#include <windowsx.h>
#include <CommCtrl.h>
#pragma comment(lib, "comctl32.lib")
#include <bitset>
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

using namespace std::chrono_literals;
using std::filesystem::path;
using std::optional;
using std::vector;

#include "SPMonitor.h"
#include "CheckForUpdate.h"
#include "RegKeyRegistryFuncs.h"

#include "..\SpaceCon\CommonToBoth.h"
#include "MiscFunctions.h"

// Global Variables:
static HINSTANCE g_hInstance;			// current instance
static HINSTANCE g_hResInst;			// Resource DLL instance
static CStringW g_AppName;				// The title bar text
static CString g_TipFmtString;			// Tooltip format string
static CString g_TipInfoFmtString;		// Tooltip Info format string

static LPCTSTR szWindowClass = _T("JD_Design_SPMONITOR_wndClass");	// the main window class name
static BOOL g_bHasCleanmgr;				// The disk cleanup application is available

static optional<CMyRegData> g_RegData;

#define UWM_TIPNOTIFY		WM_USER+1
#define UWM_RELOAD_SETTINGS	WM_USER+2
#define UWM_RESTART_ME		WM_USER+3

static HANDLE g_hNotifyThread;	// Thread handle of the notification thread that monitors for settings changes

/* The following are the names of the registry keys and values */
/* These are the registry key/value for the XP facility */
#define EXPLORER_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer")
#define EXPLORER_VAL _T("NoLowDiskSpaceChecks")

static HANDLE g_hMonitorInstance;	// Event handle to ensure only a single instance of the monitoring process (per desktop)

// The taskbar icon for the disk drive
static HICON g_DiskDriveIcon;

/* This records which drive taskbar icons are displayed (it's 1 bit per disk drive) */
static std::bitset<26> g_DriveIconDisplayed;
// Similarly, this records which drives the user doesn't want to be reminded of
static std::bitset<26> g_bNoRefreshTip;

constexpr auto AMEGABYTE{ 1024 * 1024 };

// This is the time that we (should) keep checking the space
constexpr auto POLL_TIME{ 45 * 1000 };

// This is the period after we've displayed (or refreshed) an icon when we want to re-display the tooltip
constexpr auto INITIAL_REDISPLAY_TIME{ 5 * 60 * 1000 };
constexpr auto MAX_REDISPLAY_TIME{ 2 * 60 * 60 * 1000 };
constexpr auto TOOLTIP_DISPLAY_TIME{ 10 * 1000 };

static int ResMessageBox( HWND hWnd, int ResId, LPCTSTR pCaption, const int Flags )
{
	CString sMsg( MAKEINTRESOURCE( ResId ) );
	return(MessageBox( hWnd, sMsg, pCaption, Flags ));
}

static void LoadResourceStringSpan( HINSTANCE hInstance, UINT uID, std::span<TCHAR> Buffer )
{
#if _DEBUG
	const int NumCharsLoaded =
#endif
		LoadString( hInstance, uID, Buffer.data(), Buffer.size() );	//-V530
	// Resource string must be present
	_ASSERT( NumCharsLoaded != 0 );
}

/// <summary>
/// Handles the notification and display of low disk space warnings for a specific drive.
/// </summary>
/// <param name="UserFree">The amount of free space available to the user on the drive, in bytes.</param>
/// <param name="nid">A reference to a NOTIFYICONDATA structure used to configure and display the notification icon and tooltip.</param>
/// <param name="Total">The total storage capacity of the drive, in bytes.</param>
static void HandleDiskSpaceBelowThreshold( ULONGLONG UserFree, NOTIFYICONDATA& nid, ULONGLONG Total )
{
	const auto dNum{ nid.uID };

	/* If the redisplay period were fixed, I'd only need to save the next
	* display time, but as I've chosen to increase the period progressively,
	* I need to store both the current period for each drive and the time
	* it was last displayed.
	*/
	struct DRIVE_NI_TIME
	{
		/* The time when a drive icon was last updated,
		* so that I can re-display the balloon tooltip.
		*/
		DWORD LastDisplayedTime;
		/* The current period that the balloon tooltip redisplays */
		DWORD RedisplayPeriod;
	};

	static DRIVE_NI_TIME DriveNI[26];

	/* Set up the common (initial & refresh) aspects of the tooltip display */
	wchar_t szSpaceRemainingW[20];
	StrFormatByteSizeW( UserFree, szSpaceRemainingW, std::size( szSpaceRemainingW ) );

	/*_T("Low Disk Space Notification\nDrive %c: %s")*/
	_stprintf( nid.szInfo, g_TipInfoFmtString, _T( 'A' ) + dNum, static_cast<LPCTSTR>(szSpaceRemainingW) );	//-V111

	LoadResourceStringSpan( g_hResInst, IDS_LDS_CAPTION, nid.szInfoTitle );

	nid.uFlags = NIF_INFO;
	nid.uTimeout = TOOLTIP_DISPLAY_TIME;

	// Display an Information icon
	// Different icons for different degrees of low disk space
	// < 1MB is critical
	nid.dwInfoFlags = (UserFree < 1ULL * AMEGABYTE) ?
						NIIF_ERROR :
						// Have we got more than 15% (space to defrag) ?
						(UserFree > (15 * Total / 100)) ?
							NIIF_INFO :
							/* Insufficient to defrag - so slightly higher warning */
							NIIF_WARNING;
#pragma warning( push )
#pragma warning( disable:28159 )
	const auto tcNow = GetTickCount();
#pragma warning( push )

	/* Are we already displaying this drive's icon? */
	if ( !g_DriveIconDisplayed[dNum] )
	{
		// No, add the icon to the taskbar

		/* Display the notification icon */
		nid.uFlags |= NIF_ICON | NIF_TIP | NIF_MESSAGE;
		nid.uCallbackMessage = UWM_TIPNOTIFY;

		// "JD Design Space Patrol: Low disk space on drive %c: %s"
		_stprintf( nid.szTip, g_TipFmtString, _T( 'A' ) + dNum, static_cast<LPCTSTR>(szSpaceRemainingW) );	//-V111

		if ( Shell_NotifyIcon( NIM_ADD, &nid ) )
		{
			/* added OK */
			/* Record when this icon appears so we can refresh the balloon tooltip after a longer delay */
			DriveNI[dNum].LastDisplayedTime = tcNow;

			/* Set the initial redisplay period */
			DriveNI[dNum].RedisplayPeriod = INITIAL_REDISPLAY_TIME;

			/* Indicate that we're displaying the icon for this drive */
			g_DriveIconDisplayed[dNum] = true;

			/* We initially want to remind the user by refreshing this tip */
			g_bNoRefreshTip[dNum] = false;
		}
		else
		{
			// Failed to add icon.

			// Since Windows 10, this branch occurs if the primary monitor DPI
			// changes; the TaskbarCreated message is sent but the icons still
			// exist, so set our flag appropriately so the next time through
			// here, we'll do the else branch.
			g_DriveIconDisplayed[dNum] = true;
		}
	}
	else
	{
		/* Already displaying; has the time period expired for a refresh? */
		if ( tcNow - DriveNI[dNum].LastDisplayedTime >= DriveNI[dNum].RedisplayPeriod )
		{
			/* It's expired, refresh it unless the user has prevented the updating for this drive. */
			if ( !g_bNoRefreshTip[dNum] )
			{
				// Don't beep in this situation - limit annoying the user
				nid.dwInfoFlags |= NIIF_NOSOUND;

				if ( Shell_NotifyIcon( NIM_MODIFY, &nid ) )
				{
					/* added OK */
					/* Record when this icon appears so we can refresh the balloon tooltip after a longer delay */
					DriveNI[dNum].LastDisplayedTime = tcNow;

					/* Increase the period so that each time takes longer */
					/* But don't wait too long */
					DriveNI[dNum].RedisplayPeriod = min( 2 * DriveNI[dNum].RedisplayPeriod, MAX_REDISPLAY_TIME );
				}
				else
				{
					/* Failed to add icon */
					MessageBeep( MB_OK );
				}
			}
		}
	}
}

/// <summary>
/// Monitors disk space for a specified drive and displays notifications when free space falls below a configured threshold.
/// </summary>
/// <param name="pDrive">A pointer to a string specifying the drive to monitor (e.g., 'C:\').</param>
/// <param name="nid">A reference to a partially pre-configured NOTIFYICONDATA structure used to configure and display the notification icon and tooltip.</param>
static void MonitorDiskSpace( LPTSTR pDrive, NOTIFYICONDATA& nid )
{
	ULARGE_INTEGER UserFree, Total, TotFree;
	if ( GetDiskFreeSpaceEx( pDrive, &UserFree, &Total, &TotFree ) )
	{
		const auto dNum{ nid.uID };

		/* Below the threshold? (and we want to alert on this drive) */
		if ( (UserFree.QuadPart < g_DriveConfig[dNum].AlarmAt) && g_DriveConfig[dNum].bCheckMe )
		{
			HandleDiskSpaceBelowThreshold( UserFree.QuadPart, nid, Total.QuadPart );
		}
		else
		{
			/* Are we displaying this drive's icon? */
			if ( g_DriveIconDisplayed[dNum] )
			{
				/* Remove it */
				if ( Shell_NotifyIcon( NIM_DELETE, &nid ) )
				{
					/* OK */

					/* Indicate that we're not displaying the icon for this drive */
					g_DriveIconDisplayed[dNum] = false;
				}
				else
				{
					//auto hr = GetLastError();
					//CString strError;
					//strError.Format( _T( "Failed to remove icon: %d" ), hr );
					//OutputDebugString( strError );
					/* Failed to remove icon */
					MessageBeep( MB_OK );
				}
			}
		}
	}
	else
	{
		/* Can't get the values! */
		_ASSERT( false );
		MessageBeep( MB_OK );
	}
}

/// <summary>
/// Handles the main timer event to monitor all disk drives and their space usage.
/// </summary>
/// <param name="hWnd">A handle to the window that will receive notifications about disk space monitoring.</param>
static void HandleMonitorTimer( HWND hWnd )
{
	const auto ReqdBufferSize = GetLogicalDriveStrings( 0, nullptr );
	vector<TCHAR> szDriveStrings( ReqdBufferSize );
	GetLogicalDriveStrings( ReqdBufferSize, szDriveStrings.data() );
	const DWORD dwDrives = GetLogicalDrives();

	/* Initialise the fixed aspects of the notification icon data */
	NOTIFYICONDATA nid;
	nid.cbSize = sizeof( nid );
	nid.hWnd = hWnd;
	nid.uFlags = 0;
	// Use the current icon
	nid.hIcon = g_DiskDriveIcon;

	WORD dNum;
	/* Loop for all disk drives on the system */
	LPTSTR pDrive;

	// Loop for valid disk drives
	for (	dNum = 0, pDrive = szDriveStrings.data();
			(dNum < std::size(g_DriveConfig)) && (pDrive[0] != _T('\0'));
			++dNum )
	{
		/* Does this drive exist? */
		if ( dwDrives & (1 << dNum ) )
		{
			const UINT drivetype = GetDriveType( pDrive );

			/* Check local hard disks only */
			if ( DRIVE_FIXED == drivetype )
			{
				nid.uID = dNum;

				MonitorDiskSpace( pDrive, nid );
			}

			/* Next drive letter */
			pDrive = &pDrive[ lstrlen( pDrive ) + 1 ];	//-V108
		}
	}
}

#if 0
static TCHAR g_szMBCaption[100];

static void CALLBACK MessageBoxTimer(HWND /*hwnd*/, UINT /*uiMsg*/, UINT_PTR /*idEvent*/, DWORD /*dwTime*/) noexcept
{
#if 0
	/* This technique doesn't work under Vista when the messagebox has focus - the whole app closes! */
	PostQuitMessage(0);
#else
	/* Find the message box window */
	HWND hMBWnd = FindWindow( MAKEINTRESOURCE(32770), g_szMBCaption );
	if (hMBWnd != NULL )
	{
		/* And end it */
		EndDialog( hMBWnd, -1 );
	}
#endif
}

static UINT TimedMessageBox( HWND hwndParent, LPCTSTR ptszMessage, LPCTSTR ptszTitle, UINT flags, DWORD dwTimeout) noexcept
{
    UINT_PTR idTimer;

	/* Save a copy of the caption text */
	lstrcpy( g_szMBCaption, ptszTitle );

    /* Set a timer to dismiss the message box. */ 
    idTimer = SetTimer(NULL, 0, dwTimeout, MessageBoxTimer);

	const UINT uiResult = MessageBox(hwndParent, ptszMessage, ptszTitle, flags);

    /* Finished with the timer. */ 
    KillTimer(NULL, idTimer);

#if 0
	MSG msg;
	/* See if there is a WM_QUIT message in the queue. If so,
	 * then you timed out. Eat the message so you don't quit the
     *  entire application.
     */ 
    if (PeekMessage(&msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE))
	{
        /* If you timed out, then return zero. */ 
        uiResult = 0;
    }
#endif

    return uiResult;
}

static int ResTimedMessageBox( HWND hWnd, int ResId, LPCTSTR pCaption, const int Flags, DWORD dwTimeout )
{
	CString sMsg;
	if ( sMsg.LoadString( g_hResInst, ResId ) )
	{
		return(TimedMessageBox( hWnd, sMsg, pCaption, Flags, dwTimeout ));
	}
	else
	{
		// What happened to the string?
		_ASSERT( false );
		return MB_OK;
	}
}
#endif
static unsigned int __stdcall MonitorChangesThread( void * param ) noexcept
{
	/* I pass the main (invisible) window handle to the thread */
	HWND hWnd = static_cast<HWND>( param );

	bool bExit = false;

	do
	{
		const DWORD Ret = WaitForMultipleObjects( std::size(g_hEvents), g_hEvents, false, INFINITE );

		switch ( Ret )
		{
		case WAIT_OBJECT_0 + EVT_REFRESH:
			/* Refresh settings */
#ifdef _DEBUG
			MessageBeep( MB_OK );
#endif
			/* Rather than read the global data here and require a critical section
			 * around all uses of that data, just tell the UI thread to do it.
			 */
			PostMessage( hWnd, UWM_RELOAD_SETTINGS, 0, 0 );
			break;

		case WAIT_OBJECT_0 + EVT_EXITTHREAD:
			/* Exit this thread */
			bExit = true;
			break;

		case WAIT_OBJECT_0 + EVT_EXITINSTANCE:
			/* Uninstalling signal, all monitoring instances (they won't have a visible UI) need to close down */
			PostMessage( hWnd, WM_CLOSE, 0, 0 );
			break;

		case WAIT_OBJECT_0 + EVT_RESTARTMONITORS:
			/* Restart monitor instance (after entering registration information) */
			PostMessage( hWnd, UWM_RESTART_ME, 0, 0 );
			break;

		default:
			/* Unexpected! */
			_ASSERT( false );
			break;
		}
	}
	while ( !bExit );

	return 0;
}

static void LoadOptimalDriveIcon()
{
	// Load the icon in the Hi-DPI aware manner
	if ( LoadIconMetric( g_hInstance, MAKEINTRESOURCE( IDI_SMALL ), LIM_SMALL, &g_DiskDriveIcon ) == S_OK )
	{
		if ( g_DiskDriveIcon != NULL )
		{
			// All OK
			return;
		}
	}

	// Shouldn't happen
	_ASSERT( false );
}

static BOOL InitInstance(HINSTANCE hInstance, int nCmdShow) noexcept
{
	g_hResInst = g_hInstance = hInstance; // Store instance handle in our global variable
// Not necessary as no separate resource DLL is used	_AtlBaseModule.SetResourceInstance( g_hResInst );

	LoadOptimalDriveIcon();

	g_RegData = GetMyRegistrationFromTheRegistry( szRegistryKey );

	InitEvents();

	bool bNoNag = false;
	/* Process command line switches */
	for ( size_t indx = 1; indx < static_cast<size_t>(__argc); ++indx )
	{
		/* Is it a switch character? */
		const TCHAR chSw = __targv[indx][0];

		if ( ( chSw == _T('/') ) || ( chSw == _T('-') ) )
		{
			switch ( reinterpret_cast<ULONG_PTR>( CharUpper( MAKEINTRESOURCE(__targv[indx][1]) ) ) )
			{
			case 'R':
				/* Called during installation to start an instance of this monitor running.
				 * Has to be done like this because this instance is being monitored
				 * (for termination) by the installer.
				 */
				{
					TCHAR szMe[_MAX_PATH];

					GetModuleFileName( NULL, szMe, std::size( szMe ) );
#ifdef _DEBUG
					MessageBox( /*hTopWnd*/NULL, szMe, _T("About to start"), MB_OK );
#endif
					/* Start with no nag option */
					ShellExecute( /*hTopWnd*/NULL, nullptr, szMe, _T("/N"), nullptr, SW_NORMAL );
				}
				/* To allow the installer to continue, we have to exit now */
				return FALSE;

			case 'N':	// No nag - called during installation
#ifdef _DEBUG
				MessageBox( GetForegroundWindow(), _T("No Nag"), g_AppName, MB_OK );
#endif
				bNoNag = true;
				break;
			}
		}
	}

	/* Is there already another monitoring instance (on this desktop) running? */
	{
		g_hMonitorInstance = CreateEvent( NULL, true, false, _T("{FB3C04D9-4A91-4728-AA0D-F157A2AF632F}") );
		const bool bMonitorInstanceAlreadyRunning = (  g_hMonitorInstance != NULL ) && ( GetLastError() == ERROR_ALREADY_EXISTS );
		if ( bMonitorInstanceAlreadyRunning )
		{
			return FALSE;
		}
	}

	/* Does this OS have the Disk Cleanup manager application? */
	TCHAR szPath[_MAX_PATH] = _T("cleanmgr.exe");
	g_bHasCleanmgr = PathFindOnPath( szPath, nullptr );

	/* Load the settings from the registry */
	LoadGlobalSettingsFromReg();

	HWND hWnd = CreateWindow(szWindowClass, g_AppName, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 350, 100, NULL, NULL, hInstance, nullptr );

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

	/* Also start the thread that checks for changes from another instance */
	UINT tid;
	g_hNotifyThread = reinterpret_cast<HANDLE>( _beginthreadex( nullptr, 0, MonitorChangesThread, hWnd, 0, &tid ) );

	return TRUE;
}

static void RemoveAllMyNotificationIcons( HWND hWnd )
{
	NOTIFYICONDATA nid;
	nid.cbSize = sizeof( nid );
	nid.hWnd = hWnd;
	nid.uFlags = 0;

	for ( nid.uID = 0; nid.uID < g_DriveIconDisplayed.size(); ++nid.uID )
	{
		/* Are we displaying an icon for this drive? */
//				if ( g_DriveIconDisplayed[nid.uID] )
// 
				// Because there's a chance that the icon is still being
				// displayed, but not identified as such, just delete all
				// possible icons.
		{
			Shell_NotifyIcon( NIM_DELETE, &nid );
		}
	}
}

//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
// This message number is sent when the taskbar is (re)created. We must assume the icons have been lost
static UINT g_TaskBarCreated = 0;

	/* Has the taskbar been (re)created? */
	if ( message == g_TaskBarCreated )
	{
		/* Assume all our icons have been lost and need to be created again */
		// As of Windows 10, this message is also sent when the primary monitor
		// DPI changes, but the taskbar icons are not lost. Therefore we have
		// to handle this scenario too.
		g_DriveIconDisplayed.reset();

		// Remove any current icons as this message is also sent for DPI
		// changes, we may need to get an updated icon.
		RemoveAllMyNotificationIcons( hWnd );

		DestroyIcon( g_DiskDriveIcon );
		LoadOptimalDriveIcon();

		// Get a refresh done quickly to reinstate any icons that were present before
		SetTimer( hWnd, 1, 1 * 1000, NULL );
	}
	else
	switch (message)
	{
	case WM_CREATE:
	   g_TaskBarCreated = RegisterWindowMessage( _T("TaskbarCreated") );

		/* Is this instance monitoring (rather than doing the configuration)? */
		if ( g_hMonitorInstance )
		{
			/* Start a timer that runs every N seconds */
			SetTimer( hWnd, 1, POLL_TIME, NULL );
		}
		break;

	case WM_TIMER:
		if ( wParam == 1 )
		{
			KillTimer( hWnd, 1 );

			HandleMonitorTimer( hWnd );

			/* Silent check for updates - periodically */
			PeriodicCheckForUpdate( hWnd, g_RegData, szRegistryKey, g_hResInst, ProductCode::SpacePatrol,
#ifdef _DEBUG
				5min
#else
				24h
#endif
			);

			/* Reset the timer to the slower period */
			SetTimer( hWnd, 1, POLL_TIME, NULL );
		}
		break;

	case WM_COMMAND:
		{
			const int wmId = LOWORD( wParam );
			// Parse the menu selections:
			switch ( wmId )
			{
			case ID_CONFIG:
				/* Invoke the config application (which requires elevation) */
			{
				/* If the current directory is not our installation directory
				 * (which is the case in the initial run), we have to fully
				 * specify the path, so just do that anyway.
				 */
				path ConfigPath{ GetModuleFilePath(NULL) };
				ConfigPath.replace_filename( _T("SPConfig.exe") );

				ShellExecute( hWnd, nullptr, ConfigPath.c_str(), nullptr, nullptr, SW_NORMAL );
			}
			break;

			case IDM_EXIT:
				DestroyWindow( hWnd );
				break;
				// Right click menu commands
			case ID_INVOKEDISKCLEANUP:
				/* Invoke the disk cleanup manager */
			{
				TCHAR pCmd[] = _T( "/dX" );

				pCmd[2] = _T( 'A' ) + static_cast<TCHAR>( lParam );	// lParam is drive number

				// Apparently MS want to remove cleanmgr from Win10, instead I
				// can invoke the UWP version via a URL:
				//	"ms-settings:storagepolicies" but I'll leave doing this
				// until it happens because there's no equivalent to specifying
				// the drive letter.
				// https://docs.microsoft.com/en-us/windows/uwp/launch-resume/launch-settings-app

				ShellExecute( hWnd, nullptr, _T( "cleanmgr.exe" ), pCmd, nullptr, SW_NORMAL );
			}
			break;

			case ID_RUNEXPLORER:	/* Invoke Explorer */
			{
				TCHAR pCmd[] = _T( "/e,A:\\" );

				pCmd[3] = _T( 'A' ) + static_cast<TCHAR>( lParam );	// lParam is drive number

				ShellExecute( hWnd, nullptr, _T( "explorer" ), pCmd, nullptr, SW_NORMAL );
			}
			break;

			case ID_EXITSPACECONTROL:
				PostMessage( hWnd, WM_CLOSE, 0, 0 );
				break;
			default:
				return DefWindowProc( hWnd, message, wParam, lParam );
			}

		}
		break;

	case WM_DESTROY:
		/* Remove the icons for any items we may be showing */
		RemoveAllMyNotificationIcons( hWnd );

		/* Notify the wait thread to exit */
		SetEvent( g_hEvents[EVT_EXITTHREAD] );

		/* Wait for it to exit */
		WaitForSingleObject( g_hNotifyThread, INFINITE );

		PostQuitMessage(0);
		break;

	case UWM_RELOAD_SETTINGS:
		LoadGlobalSettingsFromReg();

		/* Have the main poll timer expire soon so the user sees
		 * the results of any saves in a timely manner.
		 */
		SetTimer( hWnd, 1, 1*1000, NULL );
		break;

	case UWM_RESTART_ME:
		/* Clear the signal that indicates this desktop's monitoring process is running */
		CloseHandle( g_hMonitorInstance );
		g_hMonitorInstance = NULL;

		/* Start a new instance of the monitoring process */
		TCHAR szMe[_MAX_PATH];

		GetModuleFileName( NULL, szMe, std::size( szMe ) );
#ifdef _DEBUG
		MessageBox( hWnd, szMe, _T("About to restart"), MB_OK );
#endif
		ShellExecute( hWnd, nullptr, szMe, _T("/M"), nullptr, SW_NORMAL );

		/* Exit this instance */
		PostMessage( hWnd, WM_COMMAND, MAKEWPARAM( IDM_EXIT, 0 ), 0 );
		break;

#if 0
	/* Draw the UAC shield icon on the config menu item */
	case WM_MEASUREITEM:
		{
			LPMEASUREITEMSTRUCT pms = (LPMEASUREITEMSTRUCT)lParam;
			if (pms->CtlType == ODT_MENU)
			{
				pms->itemWidth  = 16;
				pms->itemHeight = 16;
				return TRUE;
			} 
		}
		break;

	case WM_DRAWITEM: 
		{
		   LPDRAWITEMSTRUCT pds = (LPDRAWITEMSTRUCT)lParam;
		   if (pds->CtlType == ODT_MENU)
		   {
			   DrawIconEx(pds->hDC, pds->rcItem.left - 15, 
				   pds->rcItem.top, 
				   (HICON)pds->itemData, 
				   16, 16, 0, NULL, DI_NORMAL);
			   return TRUE;
		   }
		}
		break; 
#endif

	// Notification from the icon
	case UWM_TIPNOTIFY:
		{
			const int DriveNum = static_cast<int>( wParam );	// wParam is the icon ID (which is the drive number)

			switch (lParam)
			{
			case WM_LBUTTONDOWN:	// left clicked
			case WM_RBUTTONDOWN:	// right clicked
				/* Display the context menu commands */
				{
					HMENU hMenu = LoadMenu( g_hResInst, MAKEINTRESOURCE( IDR_CTXT_MENU ) );
					HMENU hPop = GetSubMenu( hMenu, 0);

					// Get the current cursor position ASAP
					POINT CurPos;
					GetCursorPos( &CurPos );

#if 0
					/* Get the shield icon */
					HICON g_hShieldIcon;
					{
						// SHSTOCKICONINFO and SHGetStockIconInfo are new to 
						// shell32.dll in Vista.

						SHSTOCKICONINFO sii = {0};
						sii.cbSize = sizeof(sii);
						SHGetStockIconInfo(SIID_SHIELD, SHGFI_ICON | SHGFI_SMALLICON, &sii);
						g_hShieldIcon = sii.hIcon;

						MENUITEMINFO mii = {0};
						mii.cbSize = sizeof(mii);
						mii.fMask = MIIM_BITMAP | MIIM_DATA;
						mii.hbmpItem = HBMMENU_CALLBACK;
						mii.dwItemData = (ULONG_PTR)g_hShieldIcon;
						SetMenuItemInfo( hPop, ID_CONFIG, FALSE, &mii);
					}
#endif

					/* Does this OS have cleanmgr ? */
					if ( !g_bHasCleanmgr )
					{
						EnableMenuItem( hPop, ID_INVOKEDISKCLEANUP, MF_BYCOMMAND | MF_GRAYED );
					}

					SetForegroundWindow( hWnd );

					// Display the menu
					const int nCmd = TrackPopupMenu( hPop, TPM_RIGHTBUTTON | TPM_RETURNCMD, CurPos.x, CurPos.y, 0, hWnd, nullptr );

					PostMessage( hWnd, WM_NULL, 0, 0);

					if ( nCmd != 0 )
					{
						/* If it's the cleanup or Explorer command, pass the drive number as the lParam */
						PostMessage( hWnd, WM_COMMAND, MAKEWPARAM( nCmd, 0 ),
										( nCmd == ID_INVOKEDISKCLEANUP ) || ( nCmd == ID_RUNEXPLORER) ? DriveNum : 0 );
					}
				}
				break;

			case WM_LBUTTONDBLCLK:	// double clicked
				break;

			case WM_MOUSEMOVE:
				/* Use this to update the main tooltip text that's displayed when the user holds the mouse over the icon */
				{
					NOTIFYICONDATA nid;
					nid.cbSize = sizeof( nid );
					nid.hWnd = hWnd;
					nid.uID = DriveNum;

					/* Get the current free space */
					ULARGE_INTEGER UserFree, Total, TotFree;
					static TCHAR szDrive[] = _T("A:\\");
					szDrive[0] = static_cast<TCHAR>( _T('A') + DriveNum );
					if ( GetDiskFreeSpaceEx( szDrive, &UserFree, &Total, &TotFree ) )
					{
						wchar_t szSpaceRemainingW[20];
						StrFormatByteSizeW( UserFree.QuadPart, szSpaceRemainingW, std::size( szSpaceRemainingW ) );

						/*_T("JD Design Space Patrol: Low disk space on drive %c: %s")*/
						_stprintf( nid.szTip, g_TipFmtString, _T( 'A' ) + DriveNum, static_cast<LPCTSTR>(szSpaceRemainingW) );	//-V111

						nid.uFlags = NIF_TIP;
						nid.uTimeout = TOOLTIP_DISPLAY_TIME;
						nid.dwInfoFlags = 0;

						/* Update the tip */
						Shell_NotifyIcon( NIM_MODIFY, &nid );
					}
				}
				break;

			case NIN_BALLOONUSERCLICK:
				/* User has clicked the balloon tooltip */
				if ( IDYES == ResMessageBox( hWnd, IDS_NO_LONGER_REMIND, g_AppName, MB_ICONQUESTION | MB_YESNO ) )
				{
					g_bNoRefreshTip[DriveNum] = true;
				}
				break;

			// Not much use, since this happens for both a full timeout, and if the user clicks on the X button in the tooltip
			//case NIN_BALLOONTIMEOUT:
			//	MessageBox( hWnd, "NIN_BALLOONTIMEOUT", g_AppName, MB_OK );
			//	break;
			}
		}
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
	wcex.lpfnWndProc	= WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SPMONITOR));
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= MAKEINTRESOURCE(IDC_SPMONITOR);
	wcex.lpszClassName	= szWindowClass;
	wcex.hIconSm		= LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	return RegisterClassEx(&wcex);
}

int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE /*hPrevInstance*/,
	_In_ LPWSTR /*lpCmdLine*/,
	_In_ int nShowCmd
	)
{
	/* Debug version memory leak checking */
	_CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );

	// Initialize global strings
	std::ignore = g_AppName.LoadString( hInstance, IDS_APP_TITLE );
	std::ignore = g_TipFmtString.LoadString( hInstance, IDS_TT_TIP_FMT );
	std::ignore = g_TipInfoFmtString.LoadString( hInstance, IDS_TT_INFO_FMT );

	MyRegisterClass(hInstance);

	// Perform application initialization:
	if (!InitInstance (hInstance, nShowCmd))
	{
		return FALSE;
	}

	HACCEL hAccelTable = LoadAccelerators( hInstance, MAKEINTRESOURCE( IDC_SPMONITOR ) );

	// Main message loop:
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return static_cast<int>( msg.wParam );
}
