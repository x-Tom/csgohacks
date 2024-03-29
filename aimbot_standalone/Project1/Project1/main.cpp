#include <iostream>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
using namespace Gdiplus;
#pragma comment (lib,"Gdiplus.lib")
#include <TlHelp32.h>
#include "Offsets.h"




bool line_toggle = true;
bool auto_lock = true;

//bool key_timeout = false;

const int SCREEN_WIDTH = GetSystemMetrics(SM_CXSCREEN); const int xhairx = SCREEN_WIDTH / 2;
const int SCREEN_HEIGHT = GetSystemMetrics(SM_CYSCREEN); const int xhairy = SCREEN_HEIGHT / 2;

HWND hwnd;
DWORD procId;
HANDLE hProcess;
uintptr_t clientBase;
uintptr_t engineBase;
HDC hdc;
int closest; //Used in a thread to save CPU usage.

//RECT coords;
////SetRect(coords, SCREEN_WIDTH - 50, 0, SCREEN_WIDTH, 20);
//
//
//void draw_text(HDC hdc, float x, float y, const wchar_t* text)
//{
//	Graphics graphics(hdc);
//
//	// Create a string.
//	
//	// Initialize arguments.
//	Font myFont(L"Arial", 16);
//	PointF origin(x, y);
//	SolidBrush redBrush(Color(255, 255, 0, 0));
//
//	// Draw string.
//	graphics.DrawString(
//		text,
//		11,
//		&myFont,
//		origin,
//		&redBrush);
//}




uintptr_t GetModuleBaseAddress(const wchar_t* modName) {
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, procId);
	if (hSnap != INVALID_HANDLE_VALUE) {
		MODULEENTRY32 modEntry;
		modEntry.dwSize = sizeof(modEntry);
		if (Module32First(hSnap, &modEntry)) {
			do {
				if (!wcscmp(modEntry.szModule, modName)) {
					CloseHandle(hSnap);
					return (uintptr_t)modEntry.modBaseAddr;
				}
			} while (Module32Next(hSnap, &modEntry));
		}
	}
}

template<typename T> T RPM(SIZE_T address) {
	T buffer;
	ReadProcessMemory(hProcess, (LPCVOID)address, &buffer, sizeof(T), NULL);
	return buffer;
}

template<typename T> void WPM(SIZE_T address, T buffer) {
	WriteProcessMemory(hProcess, (LPVOID)address, &buffer, sizeof(buffer), NULL);
}

class Vector3 {
public:
	float x, y, z;
	Vector3() : x(0.f), y(0.f), z(0.f) {}
	Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

	Vector3 operator-(Vector3 vec) {
		return Vector3(x - vec.x, y - vec.y, z - vec.z);
	}

};

double Vector3Magnitude(Vector3& vec) {
	return sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);
}
double Vector3GroundDistance(Vector3& vec) {
	return sqrt(vec.x * vec.x + vec.y * vec.y);
}


int getTeam(uintptr_t player) {
	return RPM<int>(player + m_iTeamNum);
}

uintptr_t GetLocalPlayer() {
	return RPM< uintptr_t>(clientBase + dwLocalPlayer);
}

uintptr_t GetPlayer(int index) {  //Each player has an index. 1-64
	return RPM< uintptr_t>(clientBase + dwEntityList + (index * 0x10) - 0x10); //We multiply the index by 0x10 to select the player we want in the entity list.
}

uintptr_t GetClientState() {
	return RPM<DWORD>(engineBase + dwClientState);
}

uintptr_t GetClientState_ViewAngles() {
	return RPM<DWORD>(GetClientState() + dwClientState_ViewAngles);
}

void WriteClientState_ViewAngles(float x_angle, float y_angle) {
	WPM<DWORD>(GetClientState_ViewAngles(), x_angle);
	WPM<DWORD>(GetClientState_ViewAngles() + 4, y_angle);
	/*WPM<DWORD>(GetClientState() + dwClientState_ViewAngles, x_angle);
	WPM<DWORD>(GetClientState() + dwClientState_ViewAngles + 4, y_angle);*/
}

int GetPlayerHealth(uintptr_t player) {
	return RPM<int>(player + m_iHealth);
}

Vector3 PlayerLocation(uintptr_t player) { //Stores XYZ coordinates in a Vector3.
	return RPM<Vector3>(player + m_vecOrigin);
}

bool DormantCheck(uintptr_t player) {
	return RPM<int>(player + m_bDormant);
}

Vector3 get_head(uintptr_t player) {
	struct boneMatrix_t {
		byte pad3[12];
		float x;
		byte pad1[12];
		float y;
		byte pad2[12];
		float z;
	};
	uintptr_t boneBase = RPM<uintptr_t>(player + m_dwBoneMatrix);
	boneMatrix_t boneMatrix = RPM<boneMatrix_t>(boneBase + (sizeof(boneMatrix) * 8 /*8 is the boneid for head*/));
	return Vector3(boneMatrix.x, boneMatrix.y, boneMatrix.z);
}

struct view_matrix_t {
	float matrix[16];
} vm;

struct Vector3 WorldToScreen(const struct Vector3 pos, struct view_matrix_t matrix) { //This turns 3D coordinates (ex: XYZ) int 2D coordinates (ex: XY).
	struct Vector3 out;
	float _x = matrix.matrix[0] * pos.x + matrix.matrix[1] * pos.y + matrix.matrix[2] * pos.z + matrix.matrix[3];
	float _y = matrix.matrix[4] * pos.x + matrix.matrix[5] * pos.y + matrix.matrix[6] * pos.z + matrix.matrix[7];
	out.z = matrix.matrix[12] * pos.x + matrix.matrix[13] * pos.y + matrix.matrix[14] * pos.z + matrix.matrix[15];

	_x *= 1.f / out.z;
	_y *= 1.f / out.z;

	out.x = SCREEN_WIDTH * .5f;
	out.y = SCREEN_HEIGHT * .5f;

	out.x += 0.5f * _x * SCREEN_WIDTH + 0.5f;
	out.y -= 0.5f * _y * SCREEN_HEIGHT + 0.5f;

	return out;
}

float pythag(int x1, int y1, int x2, int y2) {
	return sqrt(pow(x2 - x1, 2) + pow(y2 - y1, 2));
}

int FindClosestEnemy() {

	float Finish;
	int ClosestEntity = 1;
	Vector3 Calc = { 0, 0, 0 };
	float Closest = FLT_MAX;
	double closest_distance = FLT_MAX;
	double curr_entity_distance;
	int localTeam = getTeam(GetLocalPlayer());
	for (int i = 0; i <= 10; i++) { //Loops through all the entitys in the index 1-64.
		DWORD Entity = GetPlayer(i);
		int EnmTeam = getTeam(Entity); if (EnmTeam == localTeam) continue;
		int EnmHealth = GetPlayerHealth(Entity); if (EnmHealth < 1 || EnmHealth > 100) continue;
		int Dormant = DormantCheck(Entity); if (Dormant) continue;
		Vector3 headBone = WorldToScreen(get_head(Entity), vm);
		Vector3 EntityVector = PlayerLocation(Entity);
		Vector3 LocalPlayerVector = PlayerLocation(GetLocalPlayer());

		Vector3 displacementVector = LocalPlayerVector - EntityVector;
		//curr_entity_distance = Vector3Magnitude(displacementVector);
		curr_entity_distance = Vector3GroundDistance(displacementVector);

		if (curr_entity_distance < closest_distance) {
			closest_distance = curr_entity_distance;
			ClosestEntity = i;
		}
			
		/*Finish = pythag(headBone.x, headBone.y, xhairx, xhairy);
		if (Finish < Closest) {
			Closest = Finish;
			ClosestEntity = i;
		}
*/


		return ClosestEntity;
	}
}

void AimtoNearest(int i)
{
	DWORD Entity = GetPlayer(i);
	Vector3 EntityVector = PlayerLocation(Entity);
	Vector3 LocalPlayerVector = PlayerLocation(GetLocalPlayer());

	Vector3 displacementVector = LocalPlayerVector - EntityVector;
	//curr_entity_distance = Vector3Magnitude(displacementVector);
	double XYPlaneDistance = Vector3GroundDistance(displacementVector);

	if ((displacementVector.x / XYPlaneDistance) > 1 || (displacementVector.x / XYPlaneDistance) < -1)
		return;
	float x_r = acos(displacementVector.x / XYPlaneDistance) * 180 / 3.141592;
	x_r *= (EntityVector.y < LocalPlayerVector.y) ? -1 : 1;
	float y_r = -1 * atan(displacementVector.x / XYPlaneDistance) * 180 / 3.141592;
	WriteClientState_ViewAngles((float)x_r, (float)y_r);
}


void DrawLine(float StartX, float StartY, float EndX, float EndY) { //This function is optional for debugging.
	int a, b = 0;
	HPEN hOPen;
	HPEN hNPen = CreatePen(PS_SOLID, 2, 0x0000FF /*red*/);
	hOPen = (HPEN)SelectObject(hdc, hNPen);
	MoveToEx(hdc, StartX, StartY, NULL); //start of line
	a = LineTo(hdc, EndX, EndY); //end of line
	DeleteObject(SelectObject(hdc, hOPen));
}

void FindClosestEnemyThread() {
	while (1) {
		closest = FindClosestEnemy();
	}
}

int main() {
	hwnd = FindWindowA(NULL, "Counter-Strike: Global Offensive");
	GetWindowThreadProcessId(hwnd, &procId);
	clientBase = GetModuleBaseAddress(L"client.dll");
	engineBase = GetModuleBaseAddress(L"engine.dll");
	hProcess = OpenProcess(PROCESS_ALL_ACCESS, NULL, procId);
	hdc = GetDC(hwnd);
	CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)FindClosestEnemyThread, NULL, NULL, NULL);

	std::cout << "aimbot running..." << std::endl;

	int index = 0;

	while (!GetAsyncKeyState(VK_END)) { //press the "end" key to end the hack
		vm = RPM<view_matrix_t>(clientBase + dwViewMatrix);
		//Vector3 current_head = WorldToScreen(get_head(GetPlayer(closest)), vm);
		Vector3 current_head;

		//closest = FindClosestEnemy();

		(auto_lock) ? current_head = WorldToScreen(get_head(GetPlayer(closest)), vm) : current_head = WorldToScreen(get_head(GetPlayer(index)), vm);

		//std::cout << sizeof(long) << std::endl;


		if (line_toggle) DrawLine(xhairx, xhairy, current_head.x, current_head.y); //optinal for debugging

		if (GetAsyncKeyState(VK_MENU /*alt key*/) && current_head.z >= 0.001f /*onscreen check*/) {
			SetCursorPos(current_head.x, current_head.y); //turn off "raw input" in CSGO settings
		}

		if (GetAsyncKeyState(VK_MENU /*alt key*/)) {
			 AimtoNearest(closest);
		}
			

		if (GetAsyncKeyState(VK_F8) & 1) line_toggle = !line_toggle;
		if (GetAsyncKeyState(VK_F7) & 1) {
			auto_lock = !auto_lock;

			if (auto_lock) {
				std::cout << "AutoLock on." << std::endl;
			}
			else {
				std::cout << "AutoLock off." << std::endl;
			}

		}

		if (GetAsyncKeyState(VK_LEFT) & 0x0001) {
			index--;
			if (index < 0) index = 0;
			std::cout << index << std::endl;
			
		}
		if (GetAsyncKeyState(VK_RIGHT) & 0x0001) {
			index++;
			if (index > 10) index = 10;
			std::cout << index << std::endl;
		}

		

		
	}
}
