// KoPowerMon.h - Korean item names / labels / messages of the PowerMon plugin (UTF-8 WITH BOM!)
#pragma once
#include "KoLocalize.h"
#include "resource.h"

inline void KoPowerMonOverride(UINT id, CString& s)
{
    if (!KoUi())
        return;
    const wchar_t* k = nullptr;
    switch (id)
    {
    case IDS_PLUGIN_DESCRIPTION: k = L"간단한 전력 소비 모니터링 플러그인"; break;
    case IDS_PLUGIN_BATTERY_POWER_ITEM_NAME: k = L"배터리 전력"; break;
    case IDS_POWER_DISPLAY_LABEL: k = L"전력:"; break;
    case IDS_BATTERY_CAPACITY_DISPLAY_LABEL: k = L"용량:"; break;
    case IDS_DISCRIPTIVE_BATERY_POWER_HEADER: k = L"배터리 전력: "; break;
    case IDS_DISCRIPTIVE_BATERY_CAPACITY_HEADER: k = L"남은 용량: "; break;
    case IDS_RELOADING_BATTERY_DRIVER_TIP: k = L"배터리 드라이버 사용 권한을 다시 요청하는 중입니다.\n최신 데이터를 제공할 수 없습니다."; break;
    case IDS_BATTERY_DRIVER_HANDLER_CLEANING: k = L"배터리 드라이버 정보 수집기를 정리하는 중입니다."; break;
    case IDS_DISCRIPTIVE_BATERY_VOLTAGE_HEADER: k = L"배터리 전압: "; break;
    case IDS_BATTERY_DRIVER_REBUILDING: k = L"배터리 드라이버 정보 접근기를 만드는 중입니다."; break;
    case IDS_BATTERY_NONEXISTS: k = L"시스템 배터리가 없습니다."; break;
    case IDS_BATTERY_INFO_UNKNOWN: k = L"시스템에서 배터리 정보를 가져올 수 없습니다"; break;
    case IDS_DISCRIPTIVE_BATERY_PERCENTAGE_HEADER: k = L"배터리 퍼센트: "; break;
    case IDS_UNKNOWN: k = L"알 수 없음"; break;
    case IDS_NO_DATA_SOURCE: k = L"데이터 소스 없음"; break;
    case IDS_PLUGIN_BATTERY_PERCENTAGE_ITEM_NAME: k = L"배터리 퍼센트"; break;
    case IDS_BATTERY: k = L"배터리"; break;
    case IDS_PLUGIN_BATTERY_CAPACITY_ITEM_NAME: k = L"배터리 용량"; break;
    case IDS_PLUGIN_BATTERY_VOLTAGE_ITEM_NAME: k = L"배터리 전압"; break;
    case IDS_BATTERY_VOLTAGE_DISPLAY_LABEL: k = L"전압:"; break;
    case IDS_WARNING: k = L"경고"; break;
    case IDS_REVERT_SETTING_CONFIG: k = L"변경한 모든 내용을 초기 상태로 되돌립니다. 계속하시겠습니까?"; break;
    case IDS_PLUGIN_BATTERY_TIME_ITEM_NAME: k = L"배터리 시간"; break;
    case IDS_BATTERY_TIME_LABEL: k = L"시간:"; break;
    case IDS_PLUGIN_SMART_POWER_METER_ITEM_NAME: k = L"스마트 전력 표시"; break;
    case IDS_PLUGIN_CPU_POWER_METER_ITEM_NAME: k = L"CPU 전력"; break;
    case IDS_PLUGIN_GPU_POWER_METER_ITEM_NAME: k = L"GPU 전력"; break;
    case IDS_BATTERY_DISPLAY_LABEL: k = L"배터리:"; break;
    case IDS_DISCRIPTIVE_BATERY_TIME_HEADER: k = L"배터리 남은 시간: "; break;
    case IDS_DISCRIPTIVE_BATERY_CHARGE_TIME_HEADER: k = L"완충까지 남은 시간: "; break;
    default: break;
    }
    if (k != nullptr)
        s = k;
}
