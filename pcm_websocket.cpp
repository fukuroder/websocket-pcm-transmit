// g++ pcm_websocket.cpp -std=c++0x -O3 -lasound

#include<alsa/asoundlib.h>  // sudo apt-get install libasound2-dev
#include<iostream>
#include<sstream>
#include<fstream>
#include<string>
#include<vector>
#include<stdexcept>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<signal.h>
#include<time.h>

// ポート番号
static const uint16_t port = 10000;

class pcm_websocket_error : public std::runtime_error{
public:
    pcm_websocket_error(const std::string& msg):std::runtime_error(msg){}
};

volatile sig_atomic_t signaled = 0;
static void signal_handler(int sig)
{
    signaled = sig;
}

//
std::string get_current_time()
{
    time_t rawtime;
    struct tm* timeinfo;

    time (&rawtime);
    timeinfo = localtime (&rawtime);

    char buf[256] = {};
    strftime(buf, sizeof buf, "[%Y/%m/%d %H:%M:%S]", timeinfo);

    return buf;
}


//
std::string get_sec_websocket_key(const std::string& buffer)
{
    std::string value;
    std::istringstream recv_stream(buffer);
    while( recv_stream.eof() == false ){
        std::string key;
        recv_stream >> key;
        if( key == "Sec-WebSocket-Key:" ){
            recv_stream >> value;
            break;
        }
    }
    return value;
}

//
std::string get_sec_websocket_accept(const std::string& a)
{
    std::ostringstream command_line;
    command_line << "python get_sec_websocket_accept.py " << a;
    FILE *p_file = ::popen((char*)command_line.str().data(), "r");
    if( p_file == nullptr ){
        return "";
    }

    // 結果を取得
    char buffer[1024] = {};
    fgets(buffer, (sizeof buffer)-1,p_file);
    pclose(p_file);
    std::string result = buffer;

    // 末尾の改行を取り除く
    auto tail = result.end()-1;
    if( *tail == '\n' )result.erase(tail);

    return result;
}

//
void handshake(int srcSocket, int dstSocket)
{
    char buffer[1024] = {};
    int numrcv = recv(dstSocket, buffer, (sizeof buffer)-1, 0);
    if(numrcv <= 0) {
        return;
    }

    std::string value = get_sec_websocket_key(buffer);
    if( value.empty() ){
        return;
    }

    std::cout << get_current_time() << "[ INFO][SOCKET] Sec-WebSocket-Key:" << value << std::endl;

    std::string sha1_base64 = get_sec_websocket_accept(value);
    if( sha1_base64.empty() ){
        return;
    }

    std::cout << get_current_time() << "[ INFO][SOCKET] Sec-WebSocket-Accept:" << sha1_base64 << std::endl;

    std::stringstream send_stream;
    send_stream << "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: " << sha1_base64 << "\r\n\r\n";

    std::string send_str = send_stream.str();
    send(dstSocket, send_str.data(), send_str.length(), 0);
}

//
template<typename T> T get_payload_length(unsigned char* buf)
{
    T sum = 0;
    for(int ii = 0; ii < sizeof(T); ii++ ){
        sum = (sum<<8) + buf[ii];
    }
    return sum;
}

//
void get_pcm(int dstSocket, std::vector<uint8_t>& recv_buffer, int& header_size, int& data_size)
{
    uint8_t *buffer = recv_buffer.data();
    int numrcv = recv(dstSocket, buffer, recv_buffer.size(), 0);
    if(numrcv <= 0) {
        header_size = 0;
        data_size = 0;
        return;
    }

    int pos = 0;

    if( buffer[pos] & 0x08 ){
        // 切断
        header_size = 0;
        data_size = 0;
        return;
    }

    pos++;

    bool mask = (buffer[pos] & 0x80) != 0;

    // get payload length
    uint64_t payload_length = (buffer[pos] & 0x7F);
    pos++;

    if( payload_length == 0x7E ){
        payload_length = get_payload_length<uint16_t>((unsigned char*)(buffer+2));
        pos += sizeof(uint16_t);
    }
    else if( payload_length == 0x7F ){
        payload_length = get_payload_length<uint64_t>((unsigned char*)(buffer+2));
        pos += sizeof(uint64_t);
    }

    // マスク取得
    unsigned char making_key[4] = {};
    if( mask ){
        for( unsigned char& m : making_key) m = buffer[pos++];
    }

    if( data_size + header_size > recv_buffer.size() ){
        header_size = 0;
        data_size = 0;
        throw pcm_websocket_error("[ERROR][SOCKET] data_size + header_size > recv_buffer.size()!!!!");
        return;
    }

    header_size = pos;
    data_size = payload_length;

    while(numrcv < data_size + header_size){
        int a = recv(dstSocket, buffer+numrcv, recv_buffer.size()-numrcv, 0);
        if(a <= 0) {
            return;
        }
        numrcv += a;
    }

    // 復号化
    if( mask ){
        for( int ii = 0; ii < data_size; ii++ ){
            buffer[header_size + ii] ^= making_key[ii % 4];
        }
    }
}

int main()
{
    snd_pcm_t *pcm = nullptr;

    try
    {
        if (SIG_ERR == signal(SIGINT, signal_handler)) {
            return 0;
        }

        //const char card_id[] = "hw:CODEC"; // BEHRINGER UCA222
        const char card_id[] = "hw:ALSA"; // PWM

        std::cout << get_current_time() << "[ INFO][  ALSA]" << " sound card=" << card_id << std::endl;

        // open audio device
        if( ::snd_pcm_open(
            &pcm,
            card_id,
            SND_PCM_STREAM_PLAYBACK,
            0) )
        {
            throw pcm_websocket_error("[ERROR][  ALSA] snd_pcm_open error");
        }

        std::cout << get_current_time() << "[ INFO][  ALSA] snd_pcm_open successful" << std::endl;

        const int channels = 2;
        const int sample_rate = 44100;
        const int latency = 4096;

        std::cout << get_current_time() << "[ INFO][  ALSA] format=SND_PCM_FORMAT_S16" << " channels=" << channels
                  << " samplerate=" << sample_rate << " latency=" << latency << std::endl;

        // set parameters
        if( ::snd_pcm_set_params(
            pcm,
            SND_PCM_FORMAT_S16,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            channels,
            sample_rate,
            0,
            1.0*latency/44100*1000*1000) )
        {
            throw pcm_websocket_error("[ERROR][  ALSA] snd_pcm_set_params error");
        }

        std::cout << get_current_time() << "[ INFO][  ALSA] snd_pcm_set_params successful" << std::endl;

        // get buffer size and period size
        snd_pcm_uframes_t buffer_size = 0;
        snd_pcm_uframes_t period_size = 0;
        if( ::snd_pcm_get_params(
            pcm,
            &buffer_size,
            &period_size) )
        {
            throw pcm_websocket_error("[ERROR][  ALSA] snd_pcm_get_params error");
        }
        std::cout << get_current_time() << "[ INFO][  ALSA] snd_pcm_get_params successful" << std::endl;
        std::cout << get_current_time() << "[ INFO][  ALSA] buffersize=" << buffer_size << " periodsize=" << period_size << std::endl;

        //
        struct sockaddr_in srcAddr;
        struct sockaddr_in dstAddr;
        socklen_t dstAddrSize = sizeof(dstAddr);

        //
        memset(&srcAddr, 0, sizeof(srcAddr));
        srcAddr.sin_port = htons(port);
        srcAddr.sin_family = AF_INET;
        srcAddr.sin_addr.s_addr = htonl(INADDR_ANY);

        //
        int srcSocket = socket(AF_INET, SOCK_STREAM, 0);

        //
        bind(srcSocket, (struct sockaddr *) &srcAddr, sizeof(srcAddr));

        //
        listen(srcSocket, 1);

        struct timeval socket_timeout;
        socket_timeout.tv_sec  = 60;
        socket_timeout.tv_usec = 0;
        if (::setsockopt(srcSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&socket_timeout, sizeof(socket_timeout))!=0){
            throw pcm_websocket_error("[ERROR][SOCKET] setsockopt error");
        }

        do{
            std::cout << get_current_time() << "[ INFO][SOCKET] Waiting for connection..." << std::endl;
            int dstSocket = accept(srcSocket, (struct sockaddr *) &dstAddr, &dstAddrSize);
            if( signaled != 0 ){
                std::cout << std::endl;
                std::cout << get_current_time() << "[ INFO][SYSTEM] interrupt" << std::endl;
                break;
            }
            if( dstSocket == -1 )continue;

            std::cout << get_current_time() << "[ INFO][SOCKET] Connect from " << inet_ntoa(dstAddr.sin_addr) << std::endl;

            handshake(srcSocket, dstSocket);

            std::cout << get_current_time() << "[ INFO][SOCKET] handshake successful" << std::endl;

            std::vector<uint8_t> recv_buffer(1024*sizeof(int16_t)*2 + 14/*max header size*/);
            while(1) {
                int header_size = 0;
                int data_size = 0;
                get_pcm(dstSocket, recv_buffer, header_size, data_size);
                if( data_size == 0 )break;

                //-------------
                // write audio
                //-------------
                int err = ::snd_pcm_writei(pcm, recv_buffer.data()+header_size, data_size/2/sizeof(int16_t));
                if( err  < 0 ){
                    ::snd_pcm_recover(pcm, err, 1);
                }
                
                if( signaled != 0 ){
                    std::cout << std::endl;
                    std::cout << get_current_time() << "[ INFO][SYSTEM] interrupt" << std::endl;
                    break;
                }

                unsigned char aaa[] = {0x81,0x02,'O','K','\0'};
                send(dstSocket, aaa, 4, 0);
            }
            close(dstSocket);
            
            std::cout << get_current_time() << "[ INFO][SOCKET] Disconnect from " << inet_ntoa(dstAddr.sin_addr) << std::endl;
            if( signaled != 0 )break;

        }while(true);

        close(srcSocket);
        std::cout << get_current_time() << "[ INFO][SOCKET] close socket" << std::endl;
    }
    catch(pcm_websocket_error &ex)
    {
        std::cout << get_current_time() << " " << ex.what() << std::endl;
    }
    catch(std::exception &ex)
    {
        std::cout << get_current_time() << "[ERROR][??????] " << ex.what() << std::endl;
    }

    if(pcm != nullptr)
    {
        // close audio device
        ::snd_pcm_close(pcm);
        std::cout << get_current_time() << "[ INFO][  ALSA] snd_pcm_close" << std::endl;
    }

    return 0;
}
