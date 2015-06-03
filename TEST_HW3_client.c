#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <limits.h>

#define MSGLEN 128
#define CMD_BUFFER 128
#define DATA_BLOCK 20			//데이터는 20바이트씩 전송된다.
#define BLOCK_WITH_SEQ (DATA_BLOCK + 4)	//그러나 seq 번호를 같이 보내니 4바이트까지..
#define MAX_BUFFER 2000
#define MAX_WINDOW 500000000
#define TIME_INTERVAL 1

//연결, 명령어 관련 함수
void buildAndBindSocket(int* socketDes, struct sockaddr_in* serverAddr, char* serverIP, int portnum);
void chatting(int* socketDes, struct sockaddr_in* serverAddr, char* serverIP);
void initialConnect(int* socketDes, struct sockaddr_in* serverAddr);

//파일 전송 관련 함수
void SendData(int* socketDes, struct sockaddr_in* serverAddr, char* fname, socklen_t* server_addr_size);
int extractACK(char* packet);
int makePacket(int seqnum, int size, char* data);
int rcvACK();
void retransfer();

//파일 수신 관련 함수
void ReceiveData(int* socketDes, struct sockaddr_in* serverAddr, char* fname, socklen_t* server_addr_size);
void makeACK(int seq, char* ack_buffer);
int extractDataSeq(char* data);



void main(int argc, char* argv[])
{
	int socketDes;
	struct sockaddr_in serverAddr;	
	initialConnect(&socketDes, &serverAddr);	//이 연결은 명령어를 주고받는 연결이다.
}

//명령어를 전달하는 소켓
void initialConnect(int* socketDes, struct sockaddr_in* serverAddr)
{
	char connectDATA[MSGLEN];			//서버의 아이피나 포트를 넣을 곳
	char* TOKEN = NULL;

	char IPString[MSGLEN];				//IP를 저장하는 문자열
	char PORTString[MSGLEN];			//포트를 저장하는 문자열

	while(1)
	{
		//여기서의 명령어는 connect, quit만이 허용된다.
		printf("> ");
		fgets(connectDATA, MSGLEN, stdin);
	
		if(strncmp(connectDATA, "connect", 7) == 0)
		{
			//입력한 문자열 중 토큰 ' '을 기준으로 나누어 IP와 포트 번호를 얻음.
			strtok(connectDATA, " ");
			TOKEN = strtok(NULL, " ");
			strcpy(IPString, TOKEN);

			TOKEN = strtok(NULL, " ");
			strcpy(PORTString, TOKEN);

			//토큰으로 분리해낸 아이피와 포트를 보낸다.
			buildAndBindSocket(socketDes, serverAddr, IPString, atoi(PORTString));
		
			//연결이 끝났다면 이제 커맨드 모드로 접속
			chatting(socketDes, serverAddr, IPString);
		}


		//quit을 받으면 프로그램을 끝낸다.
		else if(strncmp(connectDATA, "quit", 4) == 0)
		{
			break;
		}
	}
}

//주어진 소켓과 포트 번호를 사용하여 서버의 소켓 정보를 바인딩한다.
void buildAndBindSocket(int* socketDes, struct sockaddr_in* serverAddr, char* serverIP, int portnum)
{
	if(((*socketDes) = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
	{	
		printf("CLIENT : SOCKET ERROR\n");
		exit(0);
	}

	memset((char*) serverAddr, 0, sizeof(*serverAddr));
	serverAddr->sin_family = AF_INET;
	serverAddr->sin_addr.s_addr = inet_addr(serverIP);
	serverAddr->sin_port = htons(portnum);

}

void chatting(int* socketDes, struct sockaddr_in* serverAddr, char* serverIP)
{
	int nread;					//데이터의 크기
	char msg[MSGLEN];			//메시지 버퍼
	char cmd_buffer[CMD_BUFFER] = {0, };	//명령어 버퍼
	char ACK_buffer[8] = {0, };		//ACK 버퍼
	char fname[MSGLEN] = {0, };		//get이나 put으로 요구하는 파일 이름

	int dataSocketDes;
	struct sockaddr_in dataAddr;		//데이터 전송을 위한 소켓
	
	
	socklen_t server_addr_size;		//상대방(서버)의 주소 소켓 길이
	socklen_t data_addr_size;		//데이터 전송에 사용할 소켓 길이
	char* TOKEN = NULL;

	int dataPort = 0;			//데이터 전송에 사용할 새 소켓의 포트 번호. 서버가 제공할 것이다.

	server_addr_size = sizeof((*serverAddr));

	//여기서 우리가 put이나 get 요청을 할 경우, 서버는 별도의 포트를 이용한 별도의 소켓을 만들고,
	//그 소켓 번호를 클라이언트에게 전달한다. 데이터 전송은 별개의 소켓에서 진행한다.
	
	//서버는 스레드를 하나 만들어 데이터를 전송한다.

	while(1)
	{
		printf("> ");
		
		//문자열을 입력받아 버퍼에 저장한다.
		if(!fgets(cmd_buffer, sizeof(cmd_buffer), stdin) < 0)
		{
			printf("ERROR : wrong input\n");
			exit(1);
		}

		//fgets는 개행 문자(\n)까지 얻어오므로 이를 제거한다.
		cmd_buffer[strlen(cmd_buffer) - 1] = '\0';

		//명령어는 get, put, close만을 허용한다.
		//그 외의 명령어는 아예 서버에게 보내지도 않을 것.
		
		//put [FILE]일 경우, 클라이언트는 서버에게서 파일을 받는다.
		if(strncmp(cmd_buffer, "get ", 4) == 0)
		{
			strtok(cmd_buffer, " ");
			TOKEN = strtok(NULL, " "); //파일명 추출
			strcpy(fname, TOKEN);
		
			//공백 문자는 sendto로 제대로 전달되지 않는다. 다른 문자로 잠깐 치환해서 보내도록 한다.
			cmd_buffer[3] = '0';
	
			//아까의 그 커맨드라인을 보냄
			nread = sendto((*socketDes), cmd_buffer, MSGLEN, 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));
		
			//서버는 우리의 get을 받을 경우, ACK를 보내고 파일 보내기 작업에 들어간다.
			//클라이언트가 ACK를 받으면 파일 받기 작업을 수행한다.	
			nread = recvfrom((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)serverAddr, &server_addr_size);

			//서버가 데이터 전송용으로 전해준 포트 번호를 받는다.			
			memcpy(&dataPort, (int*)ACK_buffer, 4);

			ACK_buffer[7] = '\0';		

			if(strcmp(ACK_buffer + 4, "ACK") == 0)
			{
				//데이터 전송용 소켓 바인딩
				buildAndBindSocket(&dataSocketDes, &dataAddr, serverIP, dataPort);
				data_addr_size = sizeof(dataAddr);	

				//파일 받기
				ReceiveData(&dataSocketDes, &dataAddr, fname, &data_addr_size);

				//파일 받기가 끝나면 데이터 전송용 소켓을 닫는다.
				close(dataSocketDes);
			}
	
		
		}

		//put [FILE]일 경우, 클라이언트는 서버에게 파일을 보낸다.
		else if(strncmp(cmd_buffer, "put ", 4) == 0)
		{
			strtok(cmd_buffer, " ");
			TOKEN = strtok(NULL, " "); //파일명 추출
			strcpy(fname, TOKEN);

			//공백 문자는 sendto로 제대로 전달되지 않는다. 다른 문자로 잠깐 치환해서 보내도록 한다.
			cmd_buffer[3] = '0';

			//아까의 그 커맨드라인을 보냄
			nread = sendto((*socketDes), cmd_buffer, MSGLEN, 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));

			//printf("%s 전송\n", cmd_buffer);
		
			//서버는 우리의 get을 받을 경우, ACK를 보내고 파일 보내기 작업에 들어간다.
			//클라이언트가 ACK를 받으면 파일 받기 작업을 수행한다.	
			nread = recvfrom((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)serverAddr, &server_addr_size);
	
			//서버가 데이터 전송용으로 전해준 포트 번호를 받는다.			
			memcpy(&dataPort, (int*)ACK_buffer, 4);

			ACK_buffer[7] = '\0';		

			if(strcmp(ACK_buffer + 4, "ACK") == 0)
			{

				//데이터 전송용 소켓 바인딩
				buildAndBindSocket(&dataSocketDes, &dataAddr, serverIP, dataPort);
				data_addr_size = sizeof(dataAddr);	

				//파일 보내기
				SendData(&dataSocketDes, &dataAddr, fname, &data_addr_size);

				//파일 받기가 끝나면 데이터 전송용 소켓을 닫는다.
				close(dataSocketDes);
			}
		}

		//close의 경우는, 소켓을 종료하고 다시 initialConnect  함수로 돌아갈 것.
		else if(strncmp(cmd_buffer, "close", 5) == 0)
		{
			printf("Disconnected.\n");
			close((*socketDes));
			break;
		}

		else printf("WRONG COMMAND.\n\n");

	}

}

//파일을 전송한다. 이 때 서버는 파일 보내는 함수를 스레드로 실행하고 있어야 함.
void SendData(int* socketDes, struct sockaddr_in* serverAddr, char* fname, socklen_t* server_addr_size)
{
	int nread;			//데이터의 크기
	char file_buffer[MAX_BUFFER];	//데이터를 담을 버퍼
	char ACK_buffer[8] = {0, };	//ACK 응답을 담을 버퍼. "ACK" + int 의 7바이트를 담을 수 있도록
	char* ACKWindow = NULL;		//ACK 처리여부 윈도우. 최대 (블록 크기(B) * 윈도우) 크기 정도의 용량 전송
					//이 윈도우는 처음에 0으로 초기화, ACKWindow[n]이 1이 된다는 것은
					//ACK(n)을 받았다는 것으로 인식한다.


	FILE* f;			//보낼 파일의 디스크립터
	int ACKresult;			//ACK 읽어본 후 결과값

	int sizeFactor = 0;		//지금까지 받아온 크기가 파일 크기의 1/10이 넘으면 프로그레스 바 증가
	int filesize;			//파일의 크기. 이것도 상대에게 보냄
	int currentSend = 0;		//현재까지 보낸 바이트

	int retcode;
	int nextSeq = 0;		//이제 보내야 할 부분 seq 인덱스

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
	

	printf("[%s](size : %f MB) is being sent.\n", fname, (double)filesize/1000000);

	nread = makePacket(nextSeq, 4, file_buffer);	//앞에 0 seq을 넣어보냄.	

	sleep(2);

	//파일명과 파일 크기를 보낸다.
	nread = sendto((*socketDes), file_buffer, nread, 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));
	//이때는 ACK 0의 대답을 받아내야 한다.

	//progress bar
	putchar('[');
	
	//ACK(n) 메시지는 7바이트로 가정. ("ACK" 3바이트 + n은 정수 4바이트)
	while(ACKWindow[nextSeq] == 0)
	{
		//!!!recvfrom은 데이터를 받을 때까지 대기한다.

		nread = recvfrom((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)serverAddr, server_addr_size);
		
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

	while(!feof(f))
	{
		file_buffer[0] = '\0';
		nread = fread(file_buffer, 1, DATA_BLOCK, f);
		//파일에서 DATA_BLOCK(200)바이트 정도를 파일 버퍼로 가져온다.
		nread = makePacket(nextSeq, nread, file_buffer);	//앞에 1, 2, ..n  seq을 넣어보냄.	

		//sizeFactor가 파일 크기의 1/10을 넘어가면, 프로그레스 바 출력 후 sizeFactor 다시 0으로 초기화
		if(sizeFactor >= filesize/10)
		{
			putchar('*');
			sizeFactor = 0;
		}

		while(ACKWindow[nextSeq] == 0)
		{
			//데이터를 보낸다.
			nread = sendto((*socketDes), file_buffer, nread, 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));

			currentSend += (nread - 4);	//seq을 제외함.	
			sizeFactor += (nread - 4);

			
			//파일을 보냈으니 이젠 대답을 기다린다.
			nread = recvfrom((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)serverAddr, server_addr_size);
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
	currentSend = filesize;
	//파일이 모두 전송되었다면 파일이 끝났다는 메세지("endoffile")를 보낸다.

	strcpy(file_buffer, "endoffile");
	sendto((*socketDes), file_buffer, strlen("endoffile"), 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));	

	printf("*]\nSuccessfully transferred.\n");

	//윈도우 반환

	free(ACKWindow);

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





//데이터를 받는다. 이 때 서버는 데이터 전송하는 함수를 스레드로 실행하고 있어야 함.
void ReceiveData(int* socketDes, struct sockaddr_in* serverAddr, char* fname, socklen_t* server_addr_size)
{
	int nread;			//받은 데이터의 크기


	FILE* f;			//받을 파일의 디스크립터
	char file_buffer[MAX_BUFFER];	//데이터를 담을 버퍼
	char ACK_buffer[8] = {0, };	//ACK 응답을 보내기 위한 버퍼

	int sizeFactor = 0;		//프로그레스 바를 출력하기 위함. 파일 크기의 1/10을 넘을 때마다 바 증가
	int currentGet = 0;		//현재까지 받은 파일의 바이트
	int fileSize;			//받을 파일의 크기(서버에서 전달)

	int getSeq = -1;		//읽어들인 seq. 이것을 아래의 expectedSeq와 비교할 것이다.
	int expectedSeq = 0;		//받길 원하는 seq 부분 인덱스
	
	int finished = 0;		//이중 루프를 끝내기 위해 사용

	

	//동기화를 위한 송신 함수. 그 외에는 아무 의미 없다.
	sendto((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));
	
	makeACK(expectedSeq, ACK_buffer);	//ACK 0을 만든다.

	//파일 크기를 받는다. 성공적으로 받게 되면 expectedSeq를 1로 만든다.
	nread = recvfrom((*socketDes), file_buffer, BLOCK_WITH_SEQ, 0, (struct sockaddr*)serverAddr, server_addr_size);

	//파일의 크기를 추출한다.
	memcpy(&fileSize, (int*)(file_buffer + 4), 4);	//seq 부분을 제외하고 받는다.

	//받은 데이터에 들어있는 seq을 추출한 getseq가 현재 필요한 expectedSeq와 같아야 한다.
	while(expectedSeq != getSeq)
	{
		getSeq = extractDataSeq(file_buffer);

		//원하는 seq이었다면 ACK 0을 보낸다.
		if(getSeq == expectedSeq)
		{
			sendto((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));
		}
		else retransfer();

		
	}

	expectedSeq++;


	//파일의 크기를 추출한다.
	//memcpy(&fileSize, (int*)(file_buffer + 4), 4);	//seq 부분을 제외하고 받는다.

	printf("[%s](size : %f MB) is being received\n", fname, (double)fileSize/1000000);

	if((f = fopen(fname, "wb")) == 0)
	{
		printf("write file descriptor : FAILED\n");
		exit(0);
	}

	//progress bar
	putchar('[');

	while(!finished)
	{	
		//sizeFactor가 파일 용량의 1/10이 될 때마다 프로그레스 바 증가 후, sizeFactor 0을 초기화.
		if(sizeFactor >= (fileSize/10))
		{
			putchar('*');
			sizeFactor = 0;
		}

		makeACK(expectedSeq, ACK_buffer);	//보낼 ACK를 미리 만들어둔다.

		while(getSeq != expectedSeq)
		{
			//데이터를 받아온다.
			nread = recvfrom((*socketDes), file_buffer, BLOCK_WITH_SEQ, 0, (struct sockaddr*)serverAddr, server_addr_size);
			
			//만일, 파일이 모두 전송되어서 마지막 메시지가 온 것이라면 무조건 종료.
			if(!strncmp(file_buffer, "endoffile", 9)) 
			{
				 //마지막 메시지가 end of file이면 종료
			        //printf("Successfully transferred.\n");
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
				sizeFactor += nread;
				currentGet += nread;

				//확인되었으니 ACK을 보낸다.
				sendto((*socketDes), ACK_buffer, 8, 0, (struct sockaddr*)serverAddr, sizeof((*serverAddr)));			

			}

			else retransfer();

		}	

		//다음 seq을 받아야 한다.
		expectedSeq++;
		
	}	

	printf("]\nSuccessfully transferred.\n");

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
