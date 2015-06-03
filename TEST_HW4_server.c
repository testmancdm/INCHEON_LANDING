#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define CLIENTS_NUM 30
#define MSGLEN 128
#define CMD_BUFFER 128
#define DATA_BLOCK 20			//데이터는 20바이트씩 전송된다.
#define BLOCK_WITH_SEQ (DATA_BLOCK + 4)	//그러나 seq 번호를 같이 보내니 4바이트까지..
#define MAX_BUFFER 2000
#define MAX_WINDOW 500000000
#define TIME_INTERVAL 1

//스레드를 실행할 때, 매개변수로 넣을 구조체.
typedef struct threadArgs
{
	char fname[MSGLEN];
	int dataSocketDes;
	struct sockaddr_in dataAddr;
	socklen_t data_addr_size;
	int* threadIdx;
} threadArgs;


//명령어 처리 함수
void buildAndBindSocket(int* socketDes, struct sockaddr_in* serverAddr, int portnum);
void commandProcessing(int* socketDes, int portnum);
void retransfer();

//파일 전송 관련 함수
void* SendData(void* p);	//(스레드에서 실행할 것이니 매개변수는 위 구조체로 받을 것이다.)
int extractACK(char* packet);
int makePacket(int seqnum, int size, char* data);

//파일 수신 관련 함수
void* ReceiveData(void* p);	//(스레드에서 실행할 것이니 매개변수는 위 구조체로 받을 것이다.)
void makeACK(int seq, char* ack_buffer);
int extractDataSeq(char* data);



void main(int argc, char* argv[])
{
	int socketDes;
	struct sockaddr_in my_addr;

	if(argc != 2)
	{
		printf("%s port\n", argv[0]);
		return;
	}

	//최초에 만드는 연결은 명령어 전달 연결이다. 데이터 전송 때는 별도의 소켓 사용함.
	//이 소켓은 모든 클라이언트가 동일하게 공유한다. (서버는 클라이언트들이 보내는 get, put들을 한 프로세스 내에서 처리)
	buildAndBindSocket(&socketDes, &my_addr, atoi(argv[1]));
	commandProcessing(&socketDes, atoi(argv[1]));

}


//주어진 소켓과 포트 번호를 이용해 서버의 소켓을 바인딩. 처음에는 어느 클라이언트나 공유하는 명령어 전달(put, get..)용 소켓을 만드나
//클라이언트마다 파일 송/수신을 할 때는 별도의 데이터 전송용 소켓을 만들 것이다.
void buildAndBindSocket(int* socketDes, struct sockaddr_in* serverAddr, int portnum)
{
	if(((*socketDes) = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
	{	
		printf("SERVER : SOCKET ERROR\n");
		exit(0);
	}

	memset((char*) serverAddr, 0, sizeof(*serverAddr));
	serverAddr->sin_family = AF_INET;
	serverAddr->sin_addr.s_addr = htons(INADDR_ANY);
	serverAddr->sin_port = htons(portnum);

	if((bind((*socketDes), (struct sockaddr*)serverAddr, sizeof(*serverAddr)) < 0))
	{
		printf("SERVER : BIND FAIL\n");
		exit(0);
	}
}


//여기선 명령어 연결 소켓이 작동한다. 클라이언트가 보내는 명령을 기다리고, 알맞게 처리한다.
void commandProcessing(int* socketDes, int portnum)
{
	int nread;				//데이터의 크기
	char msg[MSGLEN];			//메시지 버퍼
	char cmd_buffer[CMD_BUFFER] = {0, };	//명령어 버퍼
	char ACK_buffer[8] = {0, };		//ACK 앞에는 클라이언트에게 데이터 전송을 위한 포트를 제공한다.
	char ACKMSG[4] = {'A', 'C', 'K', '\0'};		//전송할 ACK 문자열.
	char fname[MSGLEN] = {0, };			//get이나 put으로 요구하는 파일 이름
	
	int dataPort = portnum;			//데이터 전송용 포트는 명령어 포트에서 1씩 증가한 포트값을 사용
	int threadIdx = 0;			//5개 스레드의 인덱스(스레드가 공유함)
	int threadIdxFIX;			//작업을 수행하다 인덱스가 바뀌면 안되니 고정

	pthread_t threads[CLIENTS_NUM];		//멀티클라이언트를 위한 스레드

	struct sockaddr_in serverAddr;		//새로운 데이터 소켓을 위한 서버 자신의 주소
	struct sockaddr_in clientAddr;		//명령어 연결을 위한 클라이언트  소켓
	socklen_t client_addr_size;		//상대방

	threadArgs* arg = (threadArgs*)malloc(CLIENTS_NUM * sizeof(threadArgs));	//스레드 매개변수

	client_addr_size = sizeof(clientAddr);

	printf("SERVER RUNNING...\n");

	while(1)
	{	
		//클라이언트가 보내는 메시지를 받아 처리한다.
		nread = recvfrom((*socketDes), cmd_buffer, CMD_BUFFER, 0, (struct sockaddr*)&clientAddr, &client_addr_size);
		cmd_buffer[nread] = '\0';

		//put [FILE]일 경우, 서버는 클라이언트에게 파일을 준다.
		if(strncmp(cmd_buffer, "get", 3) == 0)
		{
			strcpy(fname, cmd_buffer + 4); //파일명 추출
	
			threadIdxFIX = threadIdx;

			//포트 번호는 1씩 증가하게 하여 겹치지 않게 한다.
			dataPort++;

			//arg[].dataSocket이라는 멤버에 소켓을 바인딩한다.
			//이 소켓은 파일을 받기 위해 사용하는 소켓으로, 병렬적인 스레드 위에서 작동한다.
			buildAndBindSocket(&(arg[threadIdxFIX].dataSocketDes), &serverAddr, dataPort);

			//클라이언트에 대한 정보를 저장
			strcpy(arg[threadIdxFIX].fname, fname);
			arg[threadIdxFIX].dataAddr = clientAddr;
			arg[threadIdxFIX].data_addr_size = client_addr_size;
			arg[threadIdxFIX].threadIdx = &threadIdx;

			//스레드 개수를 하나 늘린다. 파일 수신이 끝나면 다시 줄일 것이다.
			threadIdx++;

			//이제 ACK를 보냄
			memcpy((int*)ACK_buffer, &dataPort, 4);
			strcpy(ACK_buffer + 4, ACKMSG);
			nread = sendto((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));

			//ACK를 보냈으니 클라이언트는 현재 파일 받기 작업을 시작했고,
			//이제 서버는 파일 전송 작업을 수행한다.	
			pthread_create(&threads[threadIdxFIX], NULL, SendData, (void*)&arg[threadIdxFIX]);
	
		}

		//put [FILE]일 경우, 서버는 클라이언트에게서 파일을 받는다.
		else if(strncmp(cmd_buffer, "put", 3) == 0)
		{
			strcpy(fname, cmd_buffer + 4); //파일명 추출
	
			threadIdxFIX = threadIdx;

			//포트 번호는 1씩 증가하게 하여 겹치지 않게 한다.
			dataPort++;

			//arg[].dataSocket이라는 멤버에 소켓을 바인딩한다.
			//이 소켓은 파일을 받기 위해 사용하는 소켓으로, 병렬적인 스레드 위에서 작동한다.
			buildAndBindSocket(&(arg[threadIdxFIX].dataSocketDes), &serverAddr, dataPort);

			//클라이언트에 대한 정보를 저장
			strcpy(arg[threadIdxFIX].fname, fname);
			arg[threadIdxFIX].dataAddr = clientAddr;
			arg[threadIdxFIX].data_addr_size = client_addr_size;
			arg[threadIdxFIX].threadIdx = &threadIdx;

			//스레드 개수를 하나 늘린다. 파일 수신이 끝나면 다시 줄일 것이다.
			threadIdx++;

			//이제 ACK를 보냄
			memcpy((int*)ACK_buffer, &dataPort, 4);
			strcpy(ACK_buffer + 4, ACKMSG);
			nread = sendto((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));

			//ACK를 보냈으니 클라이언트는 현재 파일 전송 작업을 시작했고,
			//이제 서버는 파일 받기 작업을 수행한다.	
			pthread_create(&threads[threadIdxFIX], NULL, ReceiveData, (void*)&arg[threadIdxFIX]);

		}

	}
}




//파일을 보내는 함수. 이 함수를 스레드를 이용해 병렬적으로 실행시킨다. 클라이언트는 이때 파일을 받는 함수를 실행해야 함.
void* SendData(void* p)
{
	int nread;			//데이터의 크기
	char file_buffer[MAX_BUFFER];	//데이터를 담을 버퍼
	char ACK_buffer[8] = {0, };	//ACK 응답을 담을 버퍼. "ACK" + int 의 7바이트를 담을 수 있도록
	char* ACKWindow = NULL;		//ACK 처리여부 윈도우. 최대 (블록 크기(B) * 윈도우) 크기 정도의 용량 전송
					//이 윈도우는 처음에 0으로 초기화, ACKWindow[n]이 1이 된다는 것은
					//ACK(n)을 받았다는 것으로 인식한다.

	FILE* f;			//보낼 파일의 디스크립터
	int ACKresult;			//ACK 읽어본 후 결과값
	int filesize;			//파일의 크기. 이것도 상대에게 보냄
	int currentSend = 0;		//지금까지 보낸 파일의 크기

	int retcode;
	int nextSeq = 0;		//이제 보내야 할 부분 seq 인덱스
	
	time_t lastTime;		
	time_t currentTime;		//1초마다 상황 출력을 위해 사용할 시간 변수


	/*======매개변수 p(threadArgs 구조채)의 정보에서 소켓과 클라이언트 정보를 가져온다=====*/
	threadArgs* args = (threadArgs*)p;
	int socketDes;
	char fname[MSGLEN];
	struct sockaddr_in clientAddr;
	socklen_t client_addr_size;
	int* threadIdx = NULL;

	socketDes = args->dataSocketDes;
	strcpy(fname, args->fname);
	clientAddr = args->dataAddr;
	client_addr_size = args->data_addr_size;
	threadIdx = args->threadIdx;
	/*===================================================================================*/


	//ACK 윈도우는 각 seq num에 대응된다. ACKWindow[5]는 seq 5를 의미.
	//이를 0으로 초기화하였다.
	ACKWindow = (char*)calloc(MAX_WINDOW, sizeof(char));

	if((f = fopen(fname, "rb")) == NULL)
		exit(1);

	//파일의 크기를 보낸다.
	fseek(f, 0L, SEEK_END);	//일단 파일 끝 부분으로 포인터 이동
	filesize = ftell(f);	//그 포인터 값이 바로 파일 크기이다.
	fseek(f, 0L, SEEK_SET);	//다시 파일 처음 부분으로 포인터 이동
	memcpy((int*)(file_buffer), &filesize, 4);	//파일 크기를 보냄
	

	nread = makePacket(nextSeq, 4, file_buffer);	//앞에 0 seq을 넣어보냄.	
	
	//동기화를 위한 수신 함수. 그 외에는  아무 의미 없다.
	nread = recvfrom(socketDes, ACK_buffer, 8, 0, (struct sockaddr*)&clientAddr, &client_addr_size);	




	//파일 크기를 보낸다.
	nread = sendto(socketDes, file_buffer, nread, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));
	//이때는 ACK 0의 대답을 받아내야 한다.

	//ACK(n) 메시지는 7바이트로 가정. ("ACK" 3바이트 + n은 정수 4바이트)
	while(ACKWindow[nextSeq] == 0)
	{
		//!!!rcvfrom은 데이터를 받을 때까지 대기한다.
		nread = recvfrom(socketDes, ACK_buffer, 8, 0, (struct sockaddr*)&clientAddr, &client_addr_size);	

		ACKresult = extractACK(ACK_buffer);
		//상대방이 보낸 ACK를 읽어들인다. 0을 얻어야 한다.

		if(ACKresult == nextSeq)
		{
			//원하는 ACK(n)이었다면 window[0] 확인. 다음 seq 준비
			ACKWindow[nextSeq] = 1;
			nextSeq++;
			break;
		}

		else retransfer();

	}

	time(&lastTime);

	while(!feof(f))
	{
		file_buffer[0] = '\0';
		nread = fread(file_buffer, 1, DATA_BLOCK, f);
		//파일에서 DATA_BLOCK(200)바이트 정도를 파일 버퍼로 가져온다.
		nread = makePacket(nextSeq, nread, file_buffer);	//앞에 1, 2, ..n  seq을 넣어보냄.	

		//현 시간 - 아까 기록한 시간의 차가 1 이상이면 1초가 흐른 것으로 간주.
		if((time(&currentTime) - lastTime) >= 1)
		{
			printf("Transfer status : send[%s][%d%c , %f MB / %f MB]\n", fname, (int)((double)currentSend * 100 /filesize),'%', (double)currentSend/1000000, (double)filesize/1000000);
			time(&lastTime);
		}
	

		while(ACKWindow[nextSeq] == 0)
		{
			//데이터를 보낸다.
			nread = sendto(socketDes, file_buffer, nread, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));
	
			currentSend += (nread - 4);

			//파일을 보냈으니 이젠 대답을 기다린다.
			nread = recvfrom(socketDes, ACK_buffer, 8, 0, (struct sockaddr*)&clientAddr, &client_addr_size);
			ACKresult = extractACK(ACK_buffer);	

			if(ACKresult == nextSeq)
			{
				ACKWindow[nextSeq] = 1;
				nextSeq++;
				break;
			}

			else retransfer();
		}
	}

	fclose(f);

	//파일이 모두 전송되었다면 파일이 끝났다는 메세지를 보낸다.

	printf("Successfully transferred.\n");

	strcpy(file_buffer, "endoffile");
	sendto(socketDes, file_buffer, strlen("endoffile"), 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));	

	//윈도우 반환
	free(ACKWindow);

	//이제 데이터 전송 스레드가 끝났다면, 스레드 수를 감소시킨다.
	//threadIdx는 위 commandProcessing 함수 안의 지역변수이다.
	(*threadIdx) = (*threadIdx) - 1;

	//서버는 데이터 전송 소켓을 여기서 닫는다.
	close(socketDes);

	return;
}


//ACK를 받아서 n을 추출한다. 만일 ACK를 제대로 추출하지 못했다면 -1 반환.
int extractACK(char* packet)
{
	char isACK[4] = {0, };
	int seqn;

	memcpy(&seqn, (int*)packet, 4);	//int(4 byte) 가져오기
	memcpy(isACK, (packet + 4), 3);	//"ACK" 문자열 가져오기

	if (strcmp(isACK, "ACK") != 0) return -1;

	else return seqn;
}

//보낼 데이터의 맨 앞에 (int 4byte) seq를 넣는다. 그 후의 크기를 반환
//데이터는 DATA_BLOCK 바이트 가량이므로, 파일 버퍼에 넣은 데이터를 대상으로 data는 파일 버퍼를 주로 전달받는다.
int makePacket(int seqnum, int size, char* data)
{
	memcpy(data + 4, data, size);
	memcpy((int*)data, &seqnum, 4);	//int(4 byte) 넣기
	
	return (4 + size);	//기존 크기 + 4(정수) 값을 반환.
}






//파일을 받는 함수. 이 함수를 스레드를 이용해 병렬적으로 실행시킨다. 클라이언트는 이때 파일을 보내는 함수를 실행해야 함.
void* ReceiveData(void* p)
{
	int nread;			//받은 데이터의 크기


	FILE* f;			//받을 파일의 디스크립터
	char file_buffer[MAX_BUFFER];	//데이터를 담을 버퍼
	char ACK_buffer[8] = {0, };	//ACK 응답을 보내기 위한 버퍼

	int currentGet = 0;		//현재까지 받은 파일의 바이트
	int fileSize;			//받을 파일의 크기(서버에서 전달)

	int getSeq = -1;		//읽어들인 seq. 이것을 아래의 expectedSeq와 비교할 것이다.
	int expectedSeq = 0;		//받길 원하는 seq 부분 인덱스
	
	int finished = 0;		//이중 루프를 끝내기 위해 사용
	
	time_t lastTime;
	time_t currentTime;		//1초마다 출력을 위해 사용할 시간 변수

	/*======매개변수 p(threadArgs 구조채)의 정보에서 소켓과 클라이언트 정보를 가져온다=====*/
	threadArgs* args = (threadArgs*)p;
	int socketDes;
	char fname[MSGLEN];
	struct sockaddr_in clientAddr;
	socklen_t client_addr_size;
	int* threadIdx = NULL;

	socketDes = args->dataSocketDes;
	strcpy(fname, args->fname);
	clientAddr = args->dataAddr;
	client_addr_size = args->data_addr_size;
	threadIdx = args->threadIdx;
	/*===================================================================================*/

	makeACK(expectedSeq, ACK_buffer);	//ACK 0을 만든다.

	nread = recvfrom(socketDes, file_buffer, BLOCK_WITH_SEQ, 0, (struct sockaddr*)&clientAddr, &client_addr_size);

	//파일명과 파일 크기를 받는다. 성공적으로 받게 되면 expectedSeq를 1로 만든다.
	
	//받은 데이터에 들어있는 seq을 추출한 getseq가 현재 필요한 expectedSeq와 같아야 한다.
	while(expectedSeq != getSeq)
	{
		getSeq = extractDataSeq(file_buffer);

		//원하는 seq이었다면 ACK 0을 보낸다.
		if(getSeq == expectedSeq)
		{
			sendto(socketDes, ACK_buffer, 8, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));
		}
		else retransfer();
	}

	expectedSeq++;

	printf("%s\n\n", file_buffer);

	//파일의 크기를 추출한다.
	memcpy(&fileSize, (int*)(file_buffer + 4), 4);	//seq 부분을 제외하고 받는다.

	if((f = fopen(fname, "wb")) == 0)
	{
		printf("write file descriptor : FAILED\n");
		exit(0);
	}

	//시작하기 전의 시간을 저장
	time(&lastTime);

	while(!finished)
	{
		makeACK(expectedSeq, ACK_buffer);	//보낼 ACK를 미리 만들어둔다.

		//현 시간 - 아까 기록한 시간의 차가 1 이상이면 1초가 흐른 것으로 간주. 1초마다 현 전송 상황 출력
		if((time(&currentTime) - lastTime) >= 1)
		{
			printf("Transfer status : recv[%s][%d%c , %f MB / %f MB]\n", fname, (int)((double)currentGet * 100 /fileSize), '%', (double)currentGet/1000000, (double)fileSize/1000000);
			time(&lastTime);
		}

		while(getSeq != expectedSeq)
		{
			//데이터를 받아온다.
			nread = recvfrom(socketDes, file_buffer, BLOCK_WITH_SEQ, 0, (struct sockaddr*)&clientAddr, &client_addr_size);
			
			//만일, 파일이 모두 전송되어서 마지막 메시지가 온 것이라면 무조건 종료.
			if(!strncmp(file_buffer, "endoffile", 9)) 
			{
				 //마지막 메시지가 end of file이면 종료
			        printf("Sucessfully transferred.\n");
		       		fclose(f);		//stream 닫기
				currentGet = fileSize;	//다 받은 것이나 마찬가지.
				finished = 1;		//이제 모든 루프를 끝낸다
				break;			//while문 빠져나가기
		        } 

			//seq을 데이터에서 추출해낸다.
			getSeq = extractDataSeq(file_buffer);
			file_buffer[nread] = 0;

			//seq을(앞 4바이트) 제외한 부분만을 파일에 쓰도록 앞 4바이트를 제외시킨다.
			nread -= 4;

			if(getSeq == expectedSeq)
			{
   
        			fwrite(file_buffer + 4, 1, nread, f); //파일로 저장
				currentGet += nread;

				//확인되었으니 ACK을 보낸다.
				sendto(socketDes, ACK_buffer, 8, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));			

			}

			else retransfer();

		}	
		
		//다음 seq을 받아야 한다.
		expectedSeq++;
		
	}	

	//이제 데이터 전송 스레드가 끝났다면, 스레드 수를 감소시킨다.
	//threadIdx는 위 commandProcessing 함수 안의 지역변수이다.
	(*threadIdx) = (*threadIdx) - 1;

	//서버는 데이터 전송 소켓을 여기서 닫는다.
	close(socketDes);
	return;
}

//seq을 받아 ACK 메시지를 만든다.
void makeACK(int seq, char* ack_buffer)
{
	char ACKMSG[4] = {'A', 'C', 'K', '\0'};

	//맨 앞에는 seq을 담고, 그 뒤에는 ACK 문자열을 담는다.
	memcpy((int*)ack_buffer, &seq, 4);
	memcpy(ack_buffer + 4, ACKMSG, 4);
}

//데이터를 받아서 seq n을 추출한다. 만일 seq를 제대로 추출하지 못했다면 -1 반환.
int extractDataSeq(char* data)
{
	int seqn;

	memcpy(&seqn, (int*)data, 4);	//int(4 byte) 가져오기
	
	return seqn;
}

//localhost에선 필요없어서 사용하지 않음
void retransfer()
{
	sleep(TIME_INTERVAL);
}
