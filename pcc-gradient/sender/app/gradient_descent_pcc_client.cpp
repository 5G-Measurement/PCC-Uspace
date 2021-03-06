#ifndef WIN32
   #include <unistd.h>
   #include <cstdlib>
   #include <cstring>
   #include <netdb.h>
#else
   #include <winsock2.h>
   #include <ws2tcpip.h>
   #include <wspiapi.h>
#endif
#include <iostream>
#include <udt.h>
#include <signal.h>

#include "pcc.h"

using namespace std;

#ifndef WIN32
void* monitor(void*);
#else
DWORD WINAPI monitor(LPVOID);
#endif

// for logging
char* file_name;
std::ofstream logger;
void segfault_sigaction(int signal, siginfo_t *si, void *arg);
void writeToLog(std::ofstream &logFile, int64_t relativeTime, double rate, double rtt,
                     int window, int64_t sent, int loss, int ack, int nack);


PCC* cchandle = NULL;

double PCC::kAlpha(1);
double PCC::kBeta(10.8);
double PCC::kExponent(0.9);
bool PCC::kPolyUtility(false);
double PCC::kFactor(0.1);
double PCC::kStep(0.05);
double PCC::kLatencyCoefficient(0);
double PCC::kInitialBoundary(0);
double PCC::kBoundaryIncrement(0);


int main(int argc, char* argv[])
{
   
   if ((argc < 4) || (0 == atoi(argv[2])))
   {
      cout << "usage: " << argv[0] << " server_ip server_port log_file [factor] [step] [alpha = 4] [beta = 1] [exponent = 2.5] [poly_utility = 1]" << endl;
      return 0;
   }

   // catching segmentation faults
   struct sigaction sa;
   memset(&sa, 0, sizeof(struct sigaction));
   sigemptyset(&sa.sa_mask);
   sa.sa_sigaction = segfault_sigaction;
   sa.sa_flags   = SA_SIGINFO;
   sigaction(SIGSEGV, &sa, NULL);

	double alpha = 1;
	double beta = 10.8;
	double exponent = 0.9;
	bool use_poly = true;
   double factor = 1.0;
   double step = 0.05;
   double latency = 0;
   double initial_boundary = 0.05;
   double boundary_increment = 0.06;


	if (argc > 4) latency = atof(argv[3]);
	if (argc > 5) factor = atof(argv[4]);
	if (argc > 6) step = atof(argv[5]);
	if (argc > 7) initial_boundary = atof(argv[6]);
	if (argc > 8) boundary_increment = atof(argv[7]);
	if (argc > 9) alpha = atof(argv[8]);
	if (argc > 10) beta = atof(argv[9]);
	if (argc > 11) exponent = atof(argv[8]);
	PCC::set_utility_params(alpha, beta, exponent, use_poly, factor, step, latency, initial_boundary, boundary_increment);
   //sleep(1500);
   // use this function to initialize the UDT library
   UDT::startup();

   struct addrinfo hints, *local, *peer;

   memset(&hints, 0, sizeof(struct addrinfo));

   hints.ai_flags = AI_PASSIVE;
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;
   //hints.ai_socktype = SOCK_DGRAM;

   if (0 != getaddrinfo(NULL, "9000", &hints, &local))
   {
      cout << "incorrect network address.\n" << endl;
      return 0;
   }

   UDTSOCKET client = UDT::socket(local->ai_family, local->ai_socktype, local->ai_protocol);

   // UDT Options
   UDT::setsockopt(client, 0, UDT_CC, new CCCFactory<PCC>, sizeof(CCCFactory<PCC>));
   //UDT::setsockopt(client, 0, UDT_MSS, new int(9000), sizeof(int));
   //UDT::setsockopt(client, 0, UDT_SNDBUF, new int(10000000), sizeof(int));
   //UDT::setsockopt(client, 0, UDP_SNDBUF, new int(10000000), sizeof(int));

   // Windows UDP issue
   // For better performance, modify HKLM\System\CurrentControlSet\Services\Afd\Parameters\FastSendDatagramThreshold
   #ifdef WIN32
      UDT::setsockopt(client, 0, UDT_MSS, new int(1052), sizeof(int));
   #endif

   /*
   UDT::setsockopt(client, 0, UDT_RENDEZVOUS, new bool(true), sizeof(bool));
   if (UDT::ERROR == UDT::bind(client, local->ai_addr, local->ai_addrlen))
   {
      cout << "bind: " << UDT::getlasterror().getErrorMessage() << endl;
      return 0;
   }
   */

   freeaddrinfo(local);

   if (0 != getaddrinfo(argv[1], argv[2], &hints, &peer))
   {
      cout << "incorrect server/peer address. " << argv[1] << ":" << argv[2] << endl;
      return 0;
   }
   file_name = argv[3];

   // connect to the server, implict bind
   if (UDT::ERROR == UDT::connect(client, peer->ai_addr, peer->ai_addrlen))
   {
      cout << "connect: " << UDT::getlasterror().getErrorMessage() << endl;
      return 0;
   }
   freeaddrinfo(peer);

   // using CC method
   int temp;
   UDT::getsockopt(client, 0, UDT_CC, &cchandle, &temp);
//   if (NULL != cchandle)
//      cchandle->setRate(1);

   int size = 100000;
   char* data = new char[size];

   #ifndef WIN32
      pthread_create(new pthread_t, NULL, monitor, &client);
   #else
      CreateThread(NULL, 0, monitor, &client, 0, NULL);
   #endif

   for (int i = 0; i < 1000000; i ++)
   {
      int ssize = 0;
      int ss;
      while (ssize < size)
      {
         if (UDT::ERROR == (ss = UDT::send(client, data + ssize, size - ssize, 0)))
         {
            cout << "send:" << UDT::getlasterror().getErrorMessage() << endl;
            break;
         }

         ssize += ss;
      }

      if (ssize < size)
         break;
   }
   logger.close();
   UDT::close(client);

   delete [] data;

   // use this function to release the UDT library
   UDT::cleanup();

   return 1;
}

#ifndef WIN32
void* monitor(void* s)
#else
DWORD WINAPI monitor(LPVOID s)
#endif
{
   logger.open(file_name);
   logger << "time," << "rate," << "rtt," << "pktsflight," << "total," << "tloss," << "acks," << "nacks\n";

   UDTSOCKET u = *(UDTSOCKET*)s;
   UDT::TRACEINFO perf;

   int i=0;
   while (true)
   {
      #ifndef WIN32
         usleep(10000);
      #else
         Sleep(1000);
      #endif
      i++;
      if(i > 10000)
      {
        exit(1); 
      }
      if (UDT::ERROR == UDT::perfmon(u, &perf))
      {
         cout << "perfmon: " << UDT::getlasterror().getErrorMessage() << endl;
         break;
      }
      writeToLog(logger, perf.msTimeStamp, perf.mbpsSendRate, perf.msRTT,
                           perf.pktFlightSize, perf.pktSentTotal, perf.pktSndLossTotal,
                           perf.pktRecvACKTotal, perf.pktRecvNAKTotal);

   }

   #ifndef WIN32
      return NULL;
   #else
      return 0;
   #endif
}

void writeToLog(std::ofstream &logFile, int64_t relativeTime, double rate, double rtt,
                     int window, int64_t sent, int loss, int ack, int nack)
{
   logFile << relativeTime << "," << rate;
   logFile << "," << rtt;
   logFile << "," << window;
   logFile << "," << sent;
   logFile << "," << loss;
   logFile << "," << ack;
   logFile << "," << nack;
   logFile << "\n";

   return;
}

void segfault_sigaction(int signal, siginfo_t *si, void *arg)
{
   logger.close();
   exit(0);
}