// dual.cpp
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <cwchar>
#include <cstdint>
#include "hidapi.h"  // hidapi 헤더 (hidapi 다운로드해서 같이 두기)
#include <mmreg.h>  // WAVEFORMATEXTENSIBLE 쓰려고 (파일 상단 include 에 추가)

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "Ole32.lib")


static hid_device* g_hidDualSense = nullptr;
static bool g_r2ResistanceEnabled = false;
static const int32_t TARGET_LEFT_ID = 70;



// 애니메이션별 사운드 딜레이 (초 단위)
static double g_delayStandDeflect = 0.0;   // StandDeflectHardExLarge
static double g_delayThrowAtk     = 0.2;   // ThrowAtk511800


//사운드 음량
static float g_masterVolume = 1.0f;

static float g_volStandDeflectEXLarge = 2.0f;
static float g_volStandDeflectHardRepartitionRival = 2.0f;
static float g_volThrowAtk511800 = 2.0f;
static float g_volThrowAtk511100 = 2.0f;
static float g_volThrowAtk511700 = 2.0f;
static float g_volMoveLeave = 1.0f;
static float g_volMoveEnter = 1.0f;





// ★ 추가: 프레임 기반 딜레이 (30fps 기준)
static double g_delayMoveLeave = 5.0  / 30.0;  // 5프레임 ≒ 0.1667초
static double g_delayMoveEnter = 13.0 / 30.0;  // 13프레임 ≒ 0.4333초
static double g_delayThrowAtk511700 = 0.0; // 
static double g_delayStandDeflectHardRepartitionRival = 0.0; // 






// 재생 트리거용 애니메이션 이름
static const wchar_t* StandDeflectHardExLarge = L"StandDeflectHardExLarge";
static const wchar_t* StandDeflectHardRepartitionRival         = L"StandDeflectHardRepartitionRival"; // rival deflect
static const wchar_t* ThrowAtk511800         = L"ThrowAtk511800"; // 그냥 간파
static const wchar_t* ThrowAtk511100         = L"ThrowAtk511100"; // 겐이치로 간파 마무리
static const wchar_t* ThrowAtk511700         = L"ThrowAtk511700"; // 겐이치로 마무리

// ★ 추가: 비전투 이동 출입 애니메이션 이름
static const wchar_t* GroundNonCombatAreaMoveLeave  = L"GroundNonCombatAreaMoveLeave";
static const wchar_t* GroundNonCombatAreaMoveEnter  = L"GroundNonCombatAreaMoveEnter";

// --------------------------------------------------
// 전역 오디오 상태 (DualSense 디바이스 + 믹스 포맷)
// --------------------------------------------------
static IMMDevice*    g_pDualSenseDevice   = nullptr;
static WAVEFORMATEX* g_pDeviceMixFormat   = nullptr;


static const uintptr_t OFFL1 = 0x48;
static const uintptr_t OFFL2 = 0x00;
static const uintptr_t OFFL3 = 0x18;
static const uintptr_t OFFL4 = 0x18;
static const uintptr_t OFFL5 = 0x158;
static const uintptr_t OFFL6 = 0x120;
static const uintptr_t OFFL7 = 0x04;

// 간단한 Release 매크로
template<typename T>
void SafeRelease(T** ppT) {
    if (ppT && *ppT) {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}


std::string WStringToUtf8(const std::wstring& w) {
    if (w.empty()) return {};

    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8,            // 출력 코드 페이지
        0,
        w.c_str(),          // 입력 wide 문자열
        -1,                 // null-terminated
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (sizeNeeded <= 0) return {};

    std::string result(sizeNeeded - 1, '\0'); // 마지막 null 제외
    WideCharToMultiByte(
        CP_UTF8,
        0,
        w.c_str(),
        -1,
        &result[0],
        sizeNeeded,
        nullptr,
        nullptr
    );
    return result;
}


// --------------------------------------------------
// 프로세스 / 모듈 찾기
// --------------------------------------------------

DWORD FindProcessId(const std::wstring& processName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(hSnap, &pe)) {
        CloseHandle(hSnap);
        return 0;
    }

    do {
        if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0) {
            DWORD pid = pe.th32ProcessID;
            CloseHandle(hSnap);
            return pid;
        }
    } while (Process32NextW(hSnap, &pe));

    CloseHandle(hSnap);
    return 0;
}

uintptr_t GetModuleBaseAddress(DWORD pid, const std::wstring& moduleName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return 0;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    if (!Module32FirstW(hSnap, &me)) {
        CloseHandle(hSnap);
        return 0;
    }

    do {
        if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) {
            uintptr_t base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            CloseHandle(hSnap);
            return base;
        }
    } while (Module32NextW(hSnap, &me));

    CloseHandle(hSnap);
    return 0;
}



bool ReadLeftWeaponId(HANDLE hProc, uintptr_t worldChrManPtr, int& outId)
{
    uintptr_t p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0;

    if (!ReadProcessMemory(hProc, (LPCVOID)worldChrManPtr, &p1, sizeof(p1), nullptr)) return false;
    if (!ReadProcessMemory(hProc, (LPCVOID)(p1 + OFFL1),      &p2, sizeof(p2), nullptr)) return false;
    if (!ReadProcessMemory(hProc, (LPCVOID)(p2 + OFFL2),      &p3, sizeof(p3), nullptr)) return false;
    if (!ReadProcessMemory(hProc, (LPCVOID)(p3 + OFFL3),      &p4, sizeof(p4), nullptr)) return false;
    if (!ReadProcessMemory(hProc, (LPCVOID)(p4 + OFFL4),      &p5, sizeof(p5), nullptr)) return false;
    if (!ReadProcessMemory(hProc, (LPCVOID)(p5 + OFFL5),      &p6, sizeof(p6), nullptr)) return false;

    int value = 0;
    if (!ReadProcessMemory(hProc, (LPCVOID)(p6 + OFFL6 + OFFL7), &value, sizeof(value), nullptr))
        return false;

    outId = value;
    return true;
}

// --------------------------------------------------
// 메모리 읽기 유틸
// --------------------------------------------------

bool ReadPtr64(HANDLE hProc, uintptr_t addr, uintptr_t& outPtr) {
    SIZE_T bytesRead = 0;
    outPtr = 0;
    if (!ReadProcessMemory(hProc,
                           reinterpret_cast<LPCVOID>(addr),
                           &outPtr,
                           sizeof(outPtr),
                           &bytesRead)) {
        return false;
    }
    return bytesRead == sizeof(outPtr);
}

bool ReadRemoteWString(HANDLE hProc, uintptr_t addr, std::wstring& out, size_t maxChars = 64) {
    if (maxChars == 0) return false;

    std::wstring buffer(maxChars, L'\0');
    SIZE_T bytesRead = 0;

    wchar_t* raw = buffer.empty() ? nullptr : &buffer[0];

    if (!ReadProcessMemory(hProc,
                           reinterpret_cast<LPCVOID>(addr),
                           raw,
                           maxChars * sizeof(wchar_t),
                           &bytesRead)) {
        return false;
    }

    size_t charsRead = bytesRead / sizeof(wchar_t);
    size_t len = 0;
    while (len < charsRead && buffer[len] != L'\0') {
        ++len;
    }
    out.assign(buffer.c_str(), len);
    return true;
}

// --------------------------------------------------
// DualSense 오디오 초기화 / 해제
// --------------------------------------------------

bool InitDualSenseAudio() {
    HRESULT hr = S_OK;

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDeviceCollection* pColl = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] MMDeviceEnumerator 생성 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        return false;
    }

    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pColl);
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] EnumAudioEndpoints 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        SafeRelease(&pEnum);
        return false;
    }

    UINT count = 0;
    pColl->GetCount(&count);

    std::cout << "[Audio] Render endpoints (active) count = " << count << "\n";

    IMMDevice* dualCandidate = nullptr;

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* pDev = nullptr;
        hr = pColl->Item(i, &pDev);
        if (FAILED(hr) || !pDev) continue;

        IPropertyStore* pStore = nullptr;
        hr = pDev->OpenPropertyStore(STGM_READ, &pStore);
        if (FAILED(hr) || !pStore) {
            SafeRelease(&pDev);
            continue;
        }

        PROPVARIANT varName;
        PropVariantInit(&varName);

        std::wstring nameW = L"(unknown)";
        hr = pStore->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR && varName.pwszVal) {
            nameW = varName.pwszVal;
        }

        DWORD state = 0xFFFFFFFF;
        hr = pDev->GetState(&state);
        if (FAILED(hr)) {
            state = 0xFFFFFFFF;
        }

        std::string nameUtf8 = WStringToUtf8(nameW);

        std::cout << "[Audio] Device[" << i << "]: \"" << nameUtf8
                  << "\" state=0x" << std::hex << state << std::dec << "\n";

        // 듀얼센스 후보 (이름 안에 DualSense / Wireless Controller 들어가면)
        if (!dualCandidate) {
            if (nameW.find(L"DualSense") != std::wstring::npos ||
                nameW.find(L"DUALSENSE") != std::wstring::npos ||
                nameW.find(L"Wireless Controller") != std::wstring::npos) {

                dualCandidate = pDev;
                dualCandidate->AddRef();
                std::cout << "[Audio] -> DualSense candidate index " << i
                          << " name=\"" << nameUtf8 << "\"\n";
            }
        }

        PropVariantClear(&varName);
        SafeRelease(&pStore);
        SafeRelease(&pDev);
    }

    // 최종 선택
    if (dualCandidate) {
        g_pDualSenseDevice = dualCandidate;
        std::cout << "[Audio] Using DualSense candidate device.\n";
    } else {
        std::wcerr << L"[Audio] DualSense endpoint를 이름으로 찾지 못했습니다. "
                   << L"기본 오디오 디바이스를 사용합니다.\n";
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDualSenseDevice);
        if (FAILED(hr) || !g_pDualSenseDevice) {
            std::wcerr << L"[Audio] 기본 오디오 디바이스도 가져오기 실패.\n";
            SafeRelease(&pColl);
            SafeRelease(&pEnum);
            return false;
        }
    }

    // 믹스 포맷 얻기
    IAudioClient* pClient = nullptr;
    hr = g_pDualSenseDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                      nullptr, (void**)&pClient);
    if (FAILED(hr) || !pClient) {
        std::wcerr << L"[Audio] IAudioClient 활성화 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        SafeRelease(&g_pDualSenseDevice);
        SafeRelease(&pColl);
        SafeRelease(&pEnum);
        return false;
    }

    hr = pClient->GetMixFormat(&g_pDeviceMixFormat);
    if (FAILED(hr) || !g_pDeviceMixFormat) {
        std::wcerr << L"[Audio] GetMixFormat 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        SafeRelease(&pClient);
        SafeRelease(&g_pDualSenseDevice);
        SafeRelease(&pColl);
        SafeRelease(&pEnum);
        return false;
    }

    std::wcout << L"[Audio] Mix format: "
               << g_pDeviceMixFormat->nChannels << L"ch, "
               << g_pDeviceMixFormat->nSamplesPerSec << L"Hz, "
               << g_pDeviceMixFormat->wBitsPerSample << L"bit\n";

    SafeRelease(&pClient);
    SafeRelease(&pColl);
    SafeRelease(&pEnum);

    return true;
}

bool InitDualSenseHid()
{
    if (hid_init() != 0) {
        std::cout << "[HID] hid_init() failed\n";
        return false;
    }

    // PS5 DualSense (USB) 기본 VID/PID
    const unsigned short VID = 0x054C; // Sony
    const unsigned short PID = 0x0CE6; // DualSense (일반 버전, USB)

    g_hidDualSense = hid_open(VID, PID, nullptr);
    if (!g_hidDualSense) {
        std::cout << "[HID] DualSense HID open failed (USB로 연결되어 있는지 확인)\n";
        return false;
    }

    hid_set_nonblocking(g_hidDualSense, 1); // 논블로킹 모드 (선택 사항)
    std::cout << "[HID] DualSense HID opened.\n";
    return true;
}

void ShutdownDualSenseHid()
{
    if (g_hidDualSense) {
        // 끌 때 저항 OFF 한 번 보내기 (선택)
        
        hid_close(g_hidDualSense);
        g_hidDualSense = nullptr;
    }
    hid_exit();
}

// audioData : MF로 디코딩한 PCM 버퍼
// volume    : 0.0f(무음) ~ 1.0f(원래) ~ 1.5f(살짝 증폭) 정도
void ApplyVolumeToAudioData(std::vector<BYTE>& audioData, float volume) {
    if (!g_pDeviceMixFormat) return;
    if (audioData.empty())    return;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 2.0f) volume = 2.0f; // 너무 크게는 막기

    WORD bits = g_pDeviceMixFormat->wBitsPerSample;
    WORD tag  = g_pDeviceMixFormat->wFormatTag;

    // ---- 32bit float 포맷 (가장 흔한 경우) ----
    bool isFloat = false;

    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (tag == WAVE_FORMAT_EXTENSIBLE) {
        auto wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(g_pDeviceMixFormat);
        if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            isFloat = true;
        }
    }

    if (isFloat && bits == 32) {
        size_t sampleCount = audioData.size() / sizeof(float);
        float* samples = reinterpret_cast<float*>(audioData.data());
        for (size_t i = 0; i < sampleCount; ++i) {
            samples[i] *= volume;
        }
        return;
    }

    // ---- 16bit PCM 포맷일 때 ----
    if (bits == 16) {
        size_t sampleCount = audioData.size() / sizeof(int16_t);
        int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
        for (size_t i = 0; i < sampleCount; ++i) {
            int32_t v = static_cast<int32_t>(samples[i] * volume);
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            samples[i] = static_cast<int16_t>(v);
        }
        return;
    }

    // 그 외 포맷은 일단 건드리지 않음
}


// 트리거 이펙트 OFF (R2 기본 상태)
static void BuildTriggerOff(uint8_t* out11)
{
    // 전부 0으로 초기화하고, effect 타입을 "Off"로 설정
    // Off = 0x05 (TriggerEffectType.Off)
    memset(out11, 0, 11);
    out11[0] = 0x05;
}

// Feedback 모드: startZone 이후로 일정 강도의 저항 적용
// startZone: 0~9, strength: 1~8 (8이 가장 강함)
static void BuildTriggerFeedback(uint8_t* out11, uint8_t startZone, uint8_t strength)
{
    if (strength == 0) {
        BuildTriggerOff(out11);
        return;
    }
    if (startZone > 9) startZone = 9;

    uint16_t activeZones = 0;
    uint32_t forceZones  = 0;

    // 내부 strength 값은 0~7, 외부 파라미터는 1~8 이라고 가정
    uint8_t forceValue = static_cast<uint8_t>((strength - 1) & 0x07);

    for (uint8_t zone = startZone; zone < 10; ++zone) {
        activeZones |= static_cast<uint16_t>(1u << zone);
        forceZones  |= static_cast<uint32_t>(forceValue << (3 * zone));
    }

    memset(out11, 0, 11);

    // Feedback 타입 = 0x21
    out11[0] = 0x21;

    // 활성 구간 비트마스크 (zone 0~9)
    out11[1] = static_cast<uint8_t>(activeZones & 0xFF);
    out11[2] = static_cast<uint8_t>((activeZones >> 8) & 0xFF);

    // 각 zone에 대한 strength 비트 (3비트씩 packed)
    out11[3] = static_cast<uint8_t>((forceZones >>  0) & 0xFF);
    out11[4] = static_cast<uint8_t>((forceZones >>  8) & 0xFF);
    out11[5] = static_cast<uint8_t>((forceZones >> 16) & 0xFF);
    out11[6] = static_cast<uint8_t>((forceZones >> 24) & 0xFF);
    // out11[7..10] 은 0 유지
}



void CleanupDualSenseAudio() {
    if (g_pDeviceMixFormat) {
        CoTaskMemFree(g_pDeviceMixFormat);
        g_pDeviceMixFormat = nullptr;
    }
    SafeRelease(&g_pDualSenseDevice);
}

// --------------------------------------------------
// WAV 파일을 DualSense 디바이스로 재생
// --------------------------------------------------


void SetR2Resistance(bool enable)
{
    if (!g_hidDualSense)
        return;

    uint8_t report[48];           // 1(ReportID) + 47(SetStateData)
    memset(report, 0, sizeof(report));

    report[0] = 0x02;             // HID Output Report ID (USB)

    // SetStateData 시작은 report[1]
    // 플래그 바이트 (0번 바이트)
    // bit2 = AllowRightTriggerFFB
    const uint8_t FLAG_ALLOW_RIGHT_TRIGGER = 0x04;
    report[1] = FLAG_ALLOW_RIGHT_TRIGGER;

    // RightTriggerFFB 위치: SetStateData offset 10 → report[1 + 10]
    uint8_t* rightTriggerFFB = report + 1 + 10;

    if (enable) {
        // 예: zone 4부터 끝까지 강한 저항 (튜닝해서 쓰면 됨)
        uint8_t startZone = 4;  // 0~9 중, 4 정도면 중간쯤에서 딱 걸리는 느낌
        uint8_t strength  = 8;  // 1~8, 8이 제일 강함
        BuildTriggerFeedback(rightTriggerFFB, startZone, strength);
        std::cout << "[HID] R2 resistance ON\n";
    } else {
        BuildTriggerOff(rightTriggerFFB);
        std::cout << "[HID] R2 resistance OFF\n";
    }

    int res = hid_write(g_hidDualSense, report, sizeof(report));
    if (res < 0) {
        std::cout << "[HID] hid_write failed when setting R2\n";
    }
}


bool PlayWavOnDualSense(const std::wstring& fileName, float volume = 1.0f) {
    if (!g_pDualSenseDevice || !g_pDeviceMixFormat) {
        std::wcerr << L"[Audio] DualSense device not initialized.\n";
        return false;
    }

    // 너무 짧은 시간 안에 계속 재생 요청 오면 끊겨 들릴 수 있으니 간단 쿨다운
    static ULONGLONG s_lastPlayTick = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastPlayTick < 80) { // 80ms 이내 중복 요청 무시
        return false;
    }
    s_lastPlayTick = now;

    HRESULT hr = S_OK;

    // exe 위치 기준으로 파일 경로 구성
    wchar_t modulePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        std::wcerr << L"[Audio] GetModuleFileName 실패\n";
        return false;
    }

    std::wstring basePath(modulePath, len);
    size_t pos = basePath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        basePath = basePath.substr(0, pos + 1);
    } else {
        basePath.clear();
    }

    std::wstring fullPath = basePath + fileName;

    IMFSourceReader* pReader   = nullptr;
    IMFMediaType*    pOutType  = nullptr;

    hr = MFCreateSourceReaderFromURL(fullPath.c_str(), nullptr, &pReader);
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] MFCreateSourceReaderFromURL 실패 hr=0x"
                   << std::hex << hr << std::dec << L" path=" << fullPath << L"\n";
        return false;
    }

    // 디바이스 믹스 포맷(WAVEFORMATEX) 기준으로 MediaType 구성
    hr = MFCreateMediaType(&pOutType);
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] MFCreateMediaType 실패\n";
        SafeRelease(&pReader);
        return false;
    }

    hr = MFInitMediaTypeFromWaveFormatEx(
        pOutType,
        g_pDeviceMixFormat,
        sizeof(WAVEFORMATEX) + g_pDeviceMixFormat->cbSize
    );
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] MFInitMediaTypeFromWaveFormatEx 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    hr = pReader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM,
        nullptr,
        pOutType
    );
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] SetCurrentMediaType 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    // 전체 PCM 데이터를 메모리에 디코딩
    std::vector<BYTE> audioData;

    while (true) {
        DWORD      dwFlags = 0;
        IMFSample* pSample = nullptr;

        hr = pReader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0,
            nullptr,
            &dwFlags,
            nullptr,
            &pSample
        );
        if (FAILED(hr)) {
            std::wcerr << L"[Audio] ReadSample 실패 hr=0x"
                       << std::hex << hr << std::dec << L"\n";
            SafeRelease(&pSample);
            break;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            SafeRelease(&pSample);
            break;
        }

        if (!pSample) continue;

        IMFMediaBuffer* pBuffer = nullptr;
        hr = pSample->ConvertToContiguousBuffer(&pBuffer);
        if (FAILED(hr)) {
            SafeRelease(&pBuffer);
            SafeRelease(&pSample);
            break;
        }

        BYTE* pBuf  = nullptr;
        DWORD cbBuf = 0;
        hr = pBuffer->Lock(&pBuf, nullptr, &cbBuf);
        if (SUCCEEDED(hr) && cbBuf > 0) {
            size_t oldSize = audioData.size();
            audioData.resize(oldSize + cbBuf);
            memcpy(audioData.data() + oldSize, pBuf, cbBuf);
            pBuffer->Unlock();
        }

        SafeRelease(&pBuffer);
        SafeRelease(&pSample);
    }

    if (audioData.empty()) {
        std::wcerr << L"[Audio] 디코딩된 오디오 데이터가 없습니다.\n";
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }
    ApplyVolumeToAudioData(audioData, g_masterVolume * volume);


    UINT32 bytesPerFrame = g_pDeviceMixFormat->nBlockAlign;
    if (bytesPerFrame == 0) {
        std::wcerr << L"[Audio] bytesPerFrame == 0\n";
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    UINT32 totalFrames = static_cast<UINT32>(audioData.size() / bytesPerFrame);
    if (totalFrames == 0) {
        std::wcerr << L"[Audio] totalFrames == 0\n";
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    // WASAPI 재생 (버퍼 크기/기간은 OS에게 맡긴다)
    IAudioClient*       pAudioClient  = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;

    hr = g_pDualSenseDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                      nullptr, (void**)&pAudioClient);
    if (FAILED(hr) || !pAudioClient) {
        std::wcerr << L"[Audio] IAudioClient 활성화 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    // 버퍼 기간은 0 → 시스템이 안전한 기본값 선택
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0,      // hnsBufferDuration
        0,      // hnsPeriodicity
        g_pDeviceMixFormat,
        nullptr
    );
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] IAudioClient::Initialize 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        SafeRelease(&pAudioClient);
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    UINT32 bufferFrameCount = 0;
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] GetBufferSize 실패\n";
        SafeRelease(&pAudioClient);
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    hr = pAudioClient->GetService(__uuidof(IAudioRenderClient),
                                  (void**)&pRenderClient);
    if (FAILED(hr) || !pRenderClient) {
        std::wcerr << L"[Audio] GetService(IAudioRenderClient) 실패\n";
        SafeRelease(&pAudioClient);
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        std::wcerr << L"[Audio] IAudioClient::Start 실패\n";
        SafeRelease(&pRenderClient);
        SafeRelease(&pAudioClient);
        SafeRelease(&pOutType);
        SafeRelease(&pReader);
        return false;
    }

    UINT32 framesWritten = 0;

    while (framesWritten < totalFrames) {
        UINT32 numPadding = 0;
        hr = pAudioClient->GetCurrentPadding(&numPadding);
        if (FAILED(hr)) {
            std::wcerr << L"[Audio] GetCurrentPadding 실패\n";
            break;
        }

        UINT32 framesAvailable = bufferFrameCount - numPadding;
        if (framesAvailable == 0) {
            Sleep(2);
            continue;
        }

        UINT32 framesToWrite = totalFrames - framesWritten;
        if (framesToWrite > framesAvailable) {
            framesToWrite = framesAvailable;
        }

        BYTE* pData = nullptr;
        hr = pRenderClient->GetBuffer(framesToWrite, &pData);
        if (FAILED(hr) || !pData) {
            std::wcerr << L"[Audio] GetBuffer 실패\n";
            break;
        }

        UINT32 bytesToCopy = framesToWrite * bytesPerFrame;
        memcpy(pData,
               audioData.data() + framesWritten * bytesPerFrame,
               bytesToCopy);

        hr = pRenderClient->ReleaseBuffer(framesToWrite, 0);
        if (FAILED(hr)) {
            std::wcerr << L"[Audio] ReleaseBuffer 실패\n";
            break;
        }

        framesWritten += framesToWrite;
    }

    // 남은 소리 다 나가게, 재생 길이만큼 잠깐 대기
    double seconds = (double)totalFrames / (double)g_pDeviceMixFormat->nSamplesPerSec;
    DWORD  sleepMs = static_cast<DWORD>(seconds * 1000.0) + 30;
    Sleep(sleepMs);

    pAudioClient->Stop();

    SafeRelease(&pRenderClient);
    SafeRelease(&pAudioClient);
    SafeRelease(&pOutType);
    SafeRelease(&pReader);

    return true;
}

void PlayWavWithDelay(const std::wstring& fileName,
                      double              delaySeconds,
                      float               volume = 1.0f)
{
    if (delaySeconds > 0.0) {
        DWORD ms = static_cast<DWORD>(delaySeconds * 1000.0);
        Sleep(ms);
    }
    PlayWavOnDualSense(fileName, volume);
}

// --------------------------------------------------
// 메인 루프: worldchrman 감시 + 애니메이션/좌수 장비 로깅
// --------------------------------------------------

int wmain() {
    // 오디오 초기화
    if (!InitDualSenseAudio()) {
        std::wcerr << L"[Dual] DualSense 오디오 초기화 실패 (그래도 메모리 로깅은 계속합니다)\n";
    }

    // ★ HID 초기화
    if (!InitDualSenseHid()) {
        std::wcerr << L"[Dual] DualSense HID 초기화 실패 (트리거 저항은 동작 안 함)\n";
    }


    SetConsoleOutputCP(CP_UTF8);

    // COM + Media Foundation 초기화
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::wcerr << L"[Init] CoInitializeEx 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        return 1;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::wcerr << L"[Init] MFStartup 실패 hr=0x"
                   << std::hex << hr << std::dec << L"\n";
        CoUninitialize();
        return 1;
    }

    std::wcout << L"[Dual] Sekiro DualSense EXE (press ESC to quit)\n";

    // 오디오 초기화
    if (!InitDualSenseAudio()) {
        std::wcerr << L"[Dual] DualSense 오디오 초기화 실패 (그래도 메모리 로깅은 계속합니다)\n";
    }

    const std::wstring procName = L"sekiro.exe";

    // 1) sekiro.exe PID
    DWORD pid = FindProcessId(procName);
    if (!pid) {
        std::wcerr << L"[ERROR] sekiro.exe not found. Run the game first.\n";
        CleanupDualSenseAudio();
        MFShutdown();
        CoUninitialize();
        std::wcout << L"Press Enter to exit...";
        std::wcin.get();
        return 1;
    }
    std::wcout << L"[INFO] PID = " << pid << L"\n";

    // 2) OpenProcess
    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) {
        std::wcerr << L"[ERROR] OpenProcess failed. GetLastError="
                   << GetLastError() << L"\n";
        CleanupDualSenseAudio();
        MFShutdown();
        CoUninitialize();
        std::wcout << L"Press Enter to exit...";
        std::wcin.get();
        return 1;
    }

    // 3) sekiro.exe base
    uintptr_t sekiroBase = GetModuleBaseAddress(pid, procName);
    if (!sekiroBase) {
        std::wcerr << L"[ERROR] Failed to get sekiro.exe base address.\n";
        CloseHandle(hProc);
        CleanupDualSenseAudio();
        MFShutdown();
        CoUninitialize();
        std::wcout << L"Press Enter to exit...";
        std::wcin.get();
        return 1;
    }
    std::wcout << L"[INFO] sekiro.exe base = 0x"
               << std::hex << sekiroBase << std::dec << L"\n";

    // 4) WorldChrMan static
    constexpr uintptr_t STATIC_OFFSET_WORLDMAN = 0x3D7A1E0; // 0x03D7A1E0
    uintptr_t worldChrStaticAddr = sekiroBase + STATIC_OFFSET_WORLDMAN;

    std::wcout << L"[INFO] WorldChrMan static addr = 0x"
               << std::hex << worldChrStaticAddr << std::dec << L"\n";

    // --- 애니메이션용 포인터 체인 오프셋 ---
    static const uintptr_t OFF1        = 0x88;
    static const uintptr_t OFF2        = 0x1FF8;
    static const uintptr_t OFF3        = 0x28;
    static const uintptr_t OFF_NAMEBUF = 0x878;

    // --- 애니메이션 ID 포인터 체인 오프셋 ---
    static const uintptr_t OFFA1 = 0x88;
    static const uintptr_t OFFA2 = 0x1FF8;
    static const uintptr_t OFFA3 = 0x10;
    static const uintptr_t OFFA4 = 0x20;    
   
    // --- 좌수 장착 장비용 포인터 체인 오프셋 ---
    static const uintptr_t OFFL1 = 0x48;
    static const uintptr_t OFFL2 = 0x00;
    static const uintptr_t OFFL3 = 0x18;
    static const uintptr_t OFFL4 = 0x18;
    static const uintptr_t OFFL5 = 0x158;
    static const uintptr_t OFFL6 = 0x120;
    static const uintptr_t OFFL7 = 0x04;


    std::wstring prevAnimName;
    bool firstAnimPrint = true;

    int32_t prevLeftId = INT32_MIN;
    bool firstLeftPrint = true;

    // 애니메이션 ID 로그용
    int32_t prevAnimId = INT32_MIN;
    bool firstAnimIdPrint = true;

    // 메인 루프
    // 메인 루프
    while (true) {
        // ESC 눌리면 종료
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            std::wcout << L"[Dual] ESC pressed, exiting...\n";
            break;
        }

        uintptr_t worldChrPtr = 0;
        if (!ReadPtr64(hProc, worldChrStaticAddr, worldChrPtr) || worldChrPtr == 0) {
            Sleep(50);
            continue;
        }
         // -------------------------
        // 애니메이션 ID 읽기
        // -------------------------
        int32_t animId = 0;
        bool    hasAnimId = false;
        {
            uintptr_t a1 = 0, a2 = 0, a3 = 0, a4 = 0;

            if (ReadPtr64(hProc, worldChrPtr + OFFA1, a1) && a1 != 0 &&
                ReadPtr64(hProc, a1 + OFFA2,          a2) && a2 != 0 &&
                ReadPtr64(hProc, a2 + OFFA3,          a3) && a3 != 0 &&
                ReadPtr64(hProc, a3 + OFFA4,          a4) && a4 != 0)
            {
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(hProc,
                                      reinterpret_cast<LPCVOID>(a4),
                                      &animId,
                                      sizeof(animId),
                                      &bytesRead) &&
                    bytesRead == sizeof(animId))
                {
                    hasAnimId = true;

                    if (firstAnimIdPrint || animId != prevAnimId) {
                        firstAnimIdPrint = false;
                        prevAnimId       = animId;

                        std::wcout << L"[ANIMID] id=" << animId
                                   << L" (addr=0x"
                                   << std::hex << a4 << std::dec << L")\n";
                    }
                }
            }
        }

        // -------------------------
        // 애니메이션 이름 읽기
        // -------------------------
        {
            uintptr_t p1 = 0, p2 = 0, p3 = 0;

            if (ReadPtr64(hProc, worldChrPtr + OFF1, p1) && p1 != 0 &&
                ReadPtr64(hProc, p1 + OFF2,        p2) && p2 != 0 &&
                ReadPtr64(hProc, p2 + OFF3,        p3) && p3 != 0)
            {
                uintptr_t nameBufAddr = p3 + OFF_NAMEBUF;
                std::wstring animName;

                if (ReadRemoteWString(hProc, nameBufAddr, animName, 64) &&
                    !animName.empty())
                {
                    if (firstAnimPrint || animName != prevAnimName) {
                        firstAnimPrint = false;
                        prevAnimName   = animName;

                        std::wcout << L"[ANIM] name=\"" << animName
                                << L"\" (buf=0x"
                                << std::hex << nameBufAddr << std::dec << L")\n";

                        if (animName == StandDeflectHardExLarge) {
                            std::wcout << L"[AUDIO] StandDeflectHardExLarge.wav 재생\n";
                            PlayWavWithDelay(L"StandDeflectHardExLarge.wav", g_delayStandDeflect, g_volStandDeflectEXLarge);
                        } else if (animName == ThrowAtk511800) {
                            std::wcout << L"[AUDIO] ThrowAtk511800.wav 재생\n";
                            PlayWavWithDelay(L"ThrowAtk511800.wav", g_delayThrowAtk, g_volThrowAtk511800);
                        } else if (animName == GroundNonCombatAreaMoveLeave) {
                            std::wcout << L"[AUDIO] GroundNonCombatAreaMoveLeave.wav 재생 (5프레임 후)\n";
                            PlayWavWithDelay(L"GroundNonCombatAreaMoveLeave.wav", g_delayMoveLeave, g_volMoveLeave);
                        } else if (animName == GroundNonCombatAreaMoveEnter) {
                            std::wcout << L"[AUDIO] GroundNonCombatAreaMoveEnter.wav 재생 (13프레임 후)\n";
                            PlayWavWithDelay(L"GroundNonCombatAreaMoveEnter.wav", g_delayMoveEnter, g_volMoveEnter);
                        } else if (animName == ThrowAtk511100) {
                            std::wcout << L"[AUDIO] ThrowAtk511100.wav 재생\n";
                            PlayWavWithDelay(L"ThrowAtk511100.wav", g_delayStandDeflect , g_volThrowAtk511100);
                        } else if (animName == ThrowAtk511700 &&
                                hasAnimId &&
                                animId == 228511700) {
                            std::wcout << L"[AUDIO] ThrowAtk511700.wav 재생 (AnimID="
                                    << animId << L")\n";
                            PlayWavWithDelay(L"ThrowAtk511700.wav", g_delayThrowAtk511700, g_volThrowAtk511700);
                        } else if (animName == StandDeflectHardRepartitionRival ) {
                            std::wcout << L"[AUDIO] StandDeflectHardRepartitionRival.wav 재생 (AnimID="
                                    << animId << L")\n";
                            PlayWavWithDelay(L"StandDeflectHardRepartitionRival.wav", g_delayStandDeflectHardRepartitionRival, g_volStandDeflectHardRepartitionRival);
                        }


                    }
                }
            }
        }

        // -------------------------
        // 좌수 장착 장비 ID 읽기
        // -------------------------
        {
            uintptr_t l1 = 0, l2 = 0, l3 = 0, l4 = 0, l5 = 0, l6 = 0;

            if (ReadPtr64(hProc, worldChrPtr + OFFL1, l1) && l1 != 0 &&
                ReadPtr64(hProc, l1 + OFFL2,         l2) && l2 != 0 &&
                ReadPtr64(hProc, l2 + OFFL3,         l3) && l3 != 0 &&
                ReadPtr64(hProc, l3 + OFFL4,         l4) && l4 != 0 &&
                ReadPtr64(hProc, l4 + OFFL5,         l5) && l5 != 0 &&
                ReadPtr64(hProc, l5 + OFFL6,         l6) && l6 != 0)
            {
                uintptr_t leftIdAddr = l6 + OFFL7;

                int32_t leftId = 0;
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(hProc,
                                    reinterpret_cast<LPCVOID>(leftIdAddr),
                                    &leftId,
                                    sizeof(leftId),
                                    &bytesRead) &&
                    bytesRead == sizeof(leftId))
                {
                    if (firstLeftPrint || leftId != prevLeftId) {
                        firstLeftPrint = false;
                        prevLeftId     = leftId;

                        std::wcout << L"[LEFT] id=" << leftId
                                << L" (addr=0x"
                                << std::hex << leftIdAddr << std::dec << L")\n";

                        bool wantResistance = (leftId == TARGET_LEFT_ID);
                        if (wantResistance != g_r2ResistanceEnabled) {
                            g_r2ResistanceEnabled = wantResistance;
                            SetR2Resistance(wantResistance);
                        }
                    }
                }
            }
        }

        Sleep(20);
    }  // end while(true)

    CloseHandle(hProc);
    CleanupDualSenseAudio();
    MFShutdown();
    CoUninitialize();
    ShutdownDualSenseHid();
    


    return 0;
}

