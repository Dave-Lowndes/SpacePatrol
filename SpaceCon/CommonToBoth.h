#pragma once
#include <span>

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

inline void LoadStringChecked( HINSTANCE hInstance, UINT uID, std::span<TCHAR> Buffer )
{
#if _DEBUG
const int NumCharsLoaded =
#endif
	LoadString( hInstance, uID, Buffer.data(), Buffer.size() );	//-V530
	// Resource string must be present
	_ASSERT( NumCharsLoaded != 0 );
}
