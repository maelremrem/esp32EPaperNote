#!/usr/bin/env python3
"""Real manager + captive handlers with IDF radio/event adapters, not RF testing."""
import test_wifi_provisioning as portal

portal.TEST = r'''
#define CHECK(c) do{if(!(c)){std::cerr<<__LINE__<<": "<<#c<<"\n";return 1;}}while(0)
httpd_req_t request(network::CaptivePortal&p,const std::string&body="") {httpd_req_t r;r.user_ctx=&p;r.headers={{"Host","192.168.4.1"},{"Origin","http://192.168.4.1"},{"Content-Type","application/x-www-form-urlencoded"},{"X-CSRF-Token",p.token_}};r.body=body;r.content_len=body.size();return r;}
int main(){
 network::WifiManager w;network::CaptivePortal p;CHECK(p.start(w));
 network::WifiScanResults results;std::string error;wifi_event_sta_scan_done_t done{};
 CHECK(w.beginPortalScan());CHECK(!w.beginPortalScan());
 scan_start_error=9;done.status=9;handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
 CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Error && error.find("9")!=std::string::npos);
 CHECK(!w.beginPortalScan());CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Error);scan_start_error=0;
 CHECK(w.beginPortalScan());scan_read_error=7;done.status=0;handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
 CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Error&&error.find("7")!=std::string::npos);scan_read_error=0;
 CHECK(w.beginPortalScan());scan_records.clear();handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
 CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Complete&&results.empty());
 CHECK(w.beginPortalScan());scan_records.clear();
 for(int i=0;i<30;++i){wifi_ap_record_t a{};std::string name="Network-"+std::to_string(i);std::memcpy(a.ssid,name.data(),name.size());a.rssi=-20-i;a.primary=1+i%11;a.authmode=i%10;scan_records.push_back(a);}
 std::memcpy(scan_records[0].ssid,"Cafe <>&",9);std::memset(scan_records[1].ssid,'X',32);
 std::memset(scan_records[2].ssid,0,32);scan_records[3].ssid[0]=1;scan_records[4].primary=0;
 std::memset(scan_records[5].ssid,0,32);std::memcpy(scan_records[5].ssid,u8"Été 東京",std::strlen(u8"Été 東京"));
 scan_records[6].ssid[0]=0xc0;scan_records[6].ssid[1]=0xaf;
 handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
 CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Complete&&results.size()==16);
 CHECK(results[0].ssid=="Cafe <>&"&&results[1].ssid.size()==32&&results[2].ssid==u8"Été 東京");
 CHECK(std::is_sorted(results.begin(),results.end(),[](const auto&a,const auto&b){return a.rssi>b.rssi;}));
 CHECK(w.beginPortalScan());barrier_delayed=true;mock_now_us+=15000001;
 CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Error && error.find("timed out")!=std::string::npos);
 CHECK(w.beginPortalScan());auto calls=scan_calls;handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
 CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Queued && scan_calls==calls);
 drain_barriers();CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Scanning && scan_calls==calls+1);
 CHECK(p.stop());handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);p.pollScan();CHECK(scan_calls==calls+1);
 CHECK(p.start(w));CHECK(w.beginPortalScan());handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
 CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Queued);drain_barriers();CHECK(w.pollPortalScan(results,error)==network::WifiScanState::Scanning);
 handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);w.pollPortalScan(results,error);
 barrier_delayed=false;
 auto post=request(p,"csrf="+p.token_);network::CaptivePortal::scan(&post);CHECK(post.status=="202 Accepted");p.pollScan();
 scan_records.clear();handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);p.pollScan();
 auto status=request(p);network::CaptivePortal::scanResults(&status);CHECK(status.response.find("complete")!=std::string::npos);
 mock_now_us+=30000001;network::CaptivePortal::scanResults(&status);CHECK(status.response.find("idle")!=std::string::npos);
 auto stale=request(p,"csrf="+p.token_);network::CaptivePortal::scan(&stale);mock_now_us+=15000001;calls=scan_calls;p.pollScan();CHECK(scan_calls==calls);
 network::CaptivePortal::scanResults(&status);CHECK(status.response.find("expired")!=std::string::npos);
 CHECK(attempts.empty());ip_event_got_ip_t ip{};handler(nullptr,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);CHECK(!w.connected()&&w.ipAddress().empty()&&bits==0);
 p.stop();std::cout<<"PASS actual scan errors/timeout/cache/bounds/UTF8/late-event barrier/no STA association\n";
}
'''
if __name__=='__main__':portal.main()
