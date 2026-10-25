#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingsockets_flat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>
using Clock = std::chrono::steady_clock;
static ISteamNetworkingSockets *Interface;
static HSteamListenSocket Listener;
static bool Failed = false;
static std::vector<HSteamNetConnection> Clients, Servers;
static std::map<HSteamNetConnection, unsigned long long> Accepted, Received;
static void Status(SteamNetConnectionStatusChangedCallback_t *Info) {
    std::fprintf(stderr,"[Direct:State] ns=%lld thread=%zu handle=%u old=%d new=%d reason=%d debug=%s\n",(long long)Clock::now().time_since_epoch().count(),std::hash<std::thread::id>{}(std::this_thread::get_id()), Info->m_hConn, Info->m_eOldState, Info->m_info.m_eState,Info->m_info.m_eEndReason,Info->m_info.m_szEndDebug);
    if (Info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && Info->m_info.m_hListenSocket == Listener) {
        Servers.push_back(Info->m_hConn);
        if (SteamAPI_ISteamNetworkingSockets_AcceptConnection(Interface,Info->m_hConn) != k_EResultOK) Failed=true;
    }
    if (Info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || Info->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) {
        Failed=true;
        char Detail[8192]{};
        SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus(Interface,Info->m_hConn,Detail,sizeof(Detail));
        std::fprintf(stderr,"[Direct:Terminal] handle=%u %s\n",Info->m_hConn,Detail);
    }
}
static void Poll() {
    SteamAPI_ISteamNetworkingSockets_RunCallbacks(Interface);
    for (auto Handle:Clients) {
        SteamNetworkingMessage_t *Message;
        while (SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection(Interface,Handle,&Message,1)>0) {
            unsigned long long Number;
            std::memcpy(&Number,Message->m_pData,sizeof(Number));
            if (Number != ++Received[Handle]) Failed=true;
            Message->Release();
        }
    }
}
int main(int Count,char **Args) {
    const int Peers=Count>1?std::atoi(Args[1]):32;
    const int Bytes=Count>2?std::atoi(Args[2]):147456;
    if (Peers<1 || Peers>32 || Bytes<8 || Bytes>524288) return 2;
    SteamNetworkingErrMsg Error{};
    if (!GameNetworkingSockets_Init(nullptr,Error)) { std::fprintf(stderr,"%s\n",Error);return 2; }
    Interface=SteamNetworkingSockets();
    SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction(SteamNetworkingUtils(),k_ESteamNetworkingSocketsDebugOutputType_Msg,[](ESteamNetworkingSocketsDebugOutputType Level,const char *Text){
        std::fprintf(stderr,"[Direct:Gns] ns=%lld thread=%zu level=%d %s\n",(long long)Clock::now().time_since_epoch().count(),std::hash<std::thread::id>{}(std::this_thread::get_id()),Level,Text);
    });
    SteamNetworkingConfigValue_t Options[6];
    Options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,(void *)Status);
    Options[1].SetInt32(k_ESteamNetworkingConfig_SendBufferSize,16*1024*1024);
    Options[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize,16*1024*1024);
    Options[3].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize,524288);
    Options[4].SetInt32(k_ESteamNetworkingConfig_SendRateMin,16*1024*1024);
    Options[5].SetInt32(k_ESteamNetworkingConfig_SendRateMax,16*1024*1024);
    SteamNetworkingIPAddr Address; Address.Clear(); Address.SetIPv4(0x7f000001,39401);
    Listener=SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP(Interface,Address,6,Options);
    for (int Index=0;Index<Peers;++Index) Clients.push_back(SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress(Interface,Address,4,Options));
    const auto Started=Clock::now();
    while (Clock::now()-Started<std::chrono::seconds(2)) { Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    if ((int)Servers.size()!=Peers || Failed) return 2;
    std::vector<unsigned char> Payload(Bytes,'a');
    std::vector<double> Credit(Peers,0);
    double GlobalCredit=0; auto Previous=Clock::now(); unsigned long long TotalAccepted=0;
    const bool Burst=Count>3 && std::atoi(Args[3])!=0;
    if (Burst) {
        for (auto Handle:Servers) {
            unsigned long long Number=1;std::memcpy(Payload.data(),&Number,sizeof(Number));
            auto Result=SteamAPI_ISteamNetworkingSockets_SendMessageToConnection(Interface,Handle,Payload.data(),Bytes,k_nSteamNetworkingSend_Reliable,nullptr);
            if (Result!=k_EResultOK) Failed=true;
            else {++Accepted[Handle];++TotalAccepted;}
        }
    }
    for (int Frame=0;Frame<(Burst?0:480) && !Failed;++Frame) {
        auto Now=Clock::now(); const double Elapsed=std::chrono::duration<double>(Now-Previous).count(); Previous=Now;
        GlobalCredit=std::min(524288.,GlobalCredit+Elapsed*Peers*6*1024*1024);
        for (int Index=0;Index<Peers;++Index) Credit[Index]=std::min(524288.,Credit[Index]+Elapsed*6*1024*1024);
        Poll();
        for (int Offset=0;Offset<Peers && !Failed;++Offset) {
            int Index=(Frame+Offset)%Peers; const auto Handle=Servers[Index];
            SteamNetConnectionRealTimeStatus_t State{};
            if (SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus(Interface,Handle,&State,0,nullptr)!=k_EResultOK) {Failed=true;break;}
            if (Credit[Index]<Bytes || GlobalCredit<Bytes || State.m_cbPendingReliable+State.m_cbSentUnackedReliable+Bytes>1048640) continue;
            auto Number=Accepted[Handle]+1;std::memcpy(Payload.data(),&Number,sizeof(Number));
            int64 BackendNumber=-1;
            auto Result=SteamAPI_ISteamNetworkingSockets_SendMessageToConnection(Interface,Handle,Payload.data(),Bytes,k_nSteamNetworkingSend_Reliable,&BackendNumber);
            if (Result!=k_EResultOK) {std::fprintf(stderr,"[Direct:SendFailure] handle=%u result=%d\n",Handle,Result);Failed=true;break;}
            ++Accepted[Handle];++TotalAccepted;Credit[Index]-=Bytes;GlobalCredit-=Bytes;
        }
        std::this_thread::sleep_until(Now+std::chrono::microseconds(16667));
    }
    const auto DrainStart=Clock::now();
    unsigned long long TotalReceived=0;
    while (!Failed && Clock::now()-DrainStart<std::chrono::seconds(20)) {
        Poll();TotalReceived=0;for (const auto &[Handle,Value]:Received) TotalReceived+=Value;
        if (TotalReceived==TotalAccepted) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr,"[Direct:Result] peers=%d bytes=%d accepted=%llu received=%llu failed=%d seconds=%.3f recovery_seconds=%.3f\n",Peers,Bytes,TotalAccepted,TotalReceived,Failed,std::chrono::duration<double>(Clock::now()-Started).count(),std::chrono::duration<double>(Clock::now()-DrainStart).count());
    for (auto Handle:Clients) SteamAPI_ISteamNetworkingSockets_CloseConnection(Interface,Handle,0,nullptr,false);
    for (auto Handle:Servers) SteamAPI_ISteamNetworkingSockets_CloseConnection(Interface,Handle,0,nullptr,false);
    SteamAPI_ISteamNetworkingSockets_CloseListenSocket(Interface,Listener);
    GameNetworkingSockets_Kill();
    return Failed || TotalAccepted!=TotalReceived;
}
