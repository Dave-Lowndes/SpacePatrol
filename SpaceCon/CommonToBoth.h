#pragma once
#include <span>
//#include <atlstr.h>

#pragma pack( push )
#pragma pack( 1 )

// This is the per-disk data that's persisted to store (the registry)
class CDriveCfg
{
public:
	bool bCheckMe;	// True = this drive is to be checked, false = don't check this drive
	ULONGLONG AlarmAt;	// Disk space warning threshold point
	CDriveCfg() noexcept
	{
		bCheckMe = false;
		AlarmAt = 0;
	}
};
#pragma pack( pop )

extern CDriveCfg g_DriveConfig[26];

extern HANDLE g_hEvents[4];
// The IDs of the events in g_hEvents
inline constexpr auto EVT_REFRESH{ 0 };
inline constexpr auto EVT_EXITTHREAD{ 1 };
inline constexpr auto EVT_EXITINSTANCE{ 2 };
inline constexpr auto EVT_RESTARTMONITORS{ 3 };

extern void LoadGlobalSettingsFromReg() noexcept;
extern void InitEvents();

/* The following are the names of the registry keys and values */
inline constexpr const TCHAR szRegistryKey[] = _T( "Software\\JD Design\\SpaceCon" );
inline constexpr LPCTSTR SETTINGS = _T( "DriveSettings" );

///// <summary>
///// Loads a string resource from a module and returns it as a wide-character string (CStringW).
///// </summary>
///// <param name="hInstance">A handle to the module containing the string resource.</param>
///// <param name="uID">The identifier of the string resource to load.</param>
///// <returns>A CStringW object containing the loaded string resource.</returns>
//inline CStringW LoadResourceString( HINSTANCE hInstance, UINT uID )
//{
//	// Use LoadStringW in the slightly unusual manner to get a pointer to
//	// the string resource without having to copy it to a buffer.
//	TCHAR* ptr;
//	const auto NumChars = LoadStringW( hInstance, uID, reinterpret_cast<LPTSTR>(&ptr), 0 );
//	// Now copy to return a CStringW
//	return { ptr, NumChars };
//}
