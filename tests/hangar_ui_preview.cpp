// Renders the production Win32 UI with synthetic data, without a game or network.
#include "../mods/hangar_manager/hangar_manager.cpp"
#include <iostream>

bool SaveWindow(HWND window,const wchar_t* path) {
    RECT rect{};GetWindowRect(window,&rect);const int w=rect.right-rect.left,h=rect.bottom-rect.top;
    HDC screen=GetDC(window),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,w,h);
    auto old=SelectObject(dc,bitmap);
    const bool captured=PrintWindow(window,dc,0)!=FALSE;
    SelectObject(dc,old);
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(w)*h*4);
    const bool read=GetDIBits(dc,bitmap,0,static_cast<UINT>(h),pixels.data(),&bi,DIB_RGB_COLORS)!=0;
    DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);
    if(!captured || !read)return false;
    BITMAPFILEHEADER header{};header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(&header),sizeof(header));
    f.write(reinterpret_cast<const char*>(&bi.bmiHeader),sizeof(BITMAPINFOHEADER));f.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    return static_cast<bool>(f);
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    g_module=GetModuleHandleW(nullptr);
    g_font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    WNDCLASSW wc{};wc.hInstance=g_module;wc.lpfnWndProc=PanelProc;wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);wc.lpszClassName=L"SDGO.HangarManager";RegisterClassW(&wc);
    // No frame is visible on the user's desktop. PrintWindow requests native
    // painting from an off-screen owned window; this is not a drawn mockup.
    EnsureManagerWindow();SetWindowTextW(g_popup,L"机库管理 — 合成数据预览");
    Rule a;a.unit=10;a.rank=1;a.subrank=2;a.name=L"扎古Ⅱ（测试机体）";a.rewardName=L"经验胶囊（200，装填完毕）";a.item=2220008;
    Rule b=a;b.unit=20;b.rank=2;b.name=L"吉姆指挥型（测试机体）";g_rules[10]=a;g_rules[20]=b;
    Plan p;p.removed=3;p.expectedItems[2220008]=15;
    for(int i=0;i<3;++i){Unit u;u.uid=100+i;u.type=i?20:10;u.slot=static_cast<uint16_t>(4+i);p.before.units.push_back(u);Action action;action.kind=Kind::Item;action.first=u.uid;p.actions.push_back(action);}
    g_scanReward=Kind::Item;Preview(p);
    SetWindowPos(g_popup,nullptr,-16000,-16000,760,520,SWP_NOACTIVATE|SWP_NOZORDER);
    ShowWindow(g_popup,SW_SHOWNOACTIVATE);UpdateWindow(g_popup);
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    const bool saved=SaveWindow(g_popup,argv[1]);DestroyWindow(g_popup);DeleteObject(g_font);
    std::cout<<(saved?"SYNTHETIC NATIVE UI CAPTURED\n":"UI CAPTURE FAILED\n");return saved?0:1;
}
