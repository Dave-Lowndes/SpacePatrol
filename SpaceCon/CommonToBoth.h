#pragma once

#pragma pack( push )
#pragma pack( 1 )

// This is the per-disk data that's persisted to store (the registry)
class CDriveCfg
{
public:
	bool bCheckMe{ false };	// True = this drive is to be checked, false = don't check this drive
	ULONGLONG AlarmAt{ 0 };	// Disk space warning threshold point
};
#pragma pack( pop )

// One for each possible drive letter
extern CDriveCfg g_DriveConfig['Z'-'A'+1];

// The IDs of the events in g_hEvents
enum EVTS { EVT_REFRESH , EVT_EXITTHREAD, EVT_EXITINSTANCE, EVT_RESTARTMONITORS };
extern HANDLE g_hEvents[EVT_RESTARTMONITORS+1];

extern void LoadGlobalSettingsFromReg() noexcept;
extern void InitEvents();

/* The following are the names of the registry keys and values */
inline constexpr const TCHAR szRegistryKey[] = _T( "Software\\JD Design\\SpaceCon" );
inline constexpr LPCTSTR SETTINGS = _T( "DriveSettings" );

inline constexpr auto AMEGABYTE{ 1024 * 1024ULL };

// This is the time that we (should) keep checking the space
inline constexpr auto POLL_TIME{ 45 * 1000 };

/* These are the registry key/value for the XP facility */
inline constexpr auto EXPLORER_KEY{ _T("Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer") };
inline constexpr auto EXPLORER_VAL{ _T("NoLowDiskSpaceChecks") };
