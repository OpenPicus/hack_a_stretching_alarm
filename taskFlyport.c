#include "taskFlyport.h"
#include "grovelib.h"
#include "sonic_ranger.h"
#include "HTTPlib.h"

#define IDLE 0
#define SET 1
#define ACTIVE 2
#define TRIGGER 3
#define TRIGGERED 4
#define STOP 5

/* XIVELY PARAMETERS */
char XivelyServer[] = "api.xively.com";
char XivelyPort[] = "80";
char XivelyAPIKey[250];
char XivelyFeedID[250];
TCP_SOCKET XivelyClient = INVALID_SOCKET;

BOOL ParamSet = FALSE;

char dbg_string[250];

void state_machine();
void blink(int n);

unsigned char state;
float distance = 0;
float distance_zero = 150;
DWORD starttime = 0, time = 0;
DWORD btt_time = 0, btt_time_0 = 0, btt_time_1 = 0;
int timeout = 0; 
int resp_code = 0;
char myData[220];
char resp_Body[150], resp_Header[150];
char cusHeader[200];
char cusPath[100];
int len = 0;

int seconds = 1800;
int afterseconds = 300;

void *ranger;

/*EE*/
unsigned int cnt_ee_i=0;
unsigned int cnt_ee_d=2;
char ee_i_d[2];

void FlyportTask()
{
	vTaskDelay(100);
	
	/* EE init */
	EEInit(EE_ADDR_DEF, HIGH_SPEED, EE_SIZE_DEF);

	/* pin init */
	IOInit(LED1_Pin,out);
	IOInit(LED2_Pin,out);
	
	// Init board
	void *board = new(IoT_Board);
	
	// GROVE device	
	ranger = new(Ranger);
	void *button = new (Dig_io, IN);
	
	// Attach Devices
	attachToBoard(board, ranger, DIG1);
	attachToBoard(board, button, DIG2);
	
	// Init of the Ranger
	configure(ranger,1);
	
	// Connection to Network
	if (WFCustomExist())
	{
		WFCustomLoad();
		_dbgwrite("Connecting to custom...\r\n");
		WFConnect(WF_CUSTOM);
	}
	else
	{
		_dbgwrite("Connecting to default...\r\n");
		WFConnect(WF_DEFAULT);
	}
	while (WFStatus != CONNECTED);
	_dbgwrite("Flyport connected... hello world!\r\n");
	vTaskDelay(200);
	if(AppConfig.networkType==WF_INFRASTRUCTURE&&WFStatus == CONNECTED)
	{
		/* Xively API Key */
		EELoadData(cnt_ee_i, ee_i_d, 1, EE_BYTE);
		EELoadString(cnt_ee_d, XivelyAPIKey, ee_i_d[0]);
		XivelyAPIKey[(int)ee_i_d[0]]='\0';
		cnt_ee_i++;
		cnt_ee_d += ee_i_d[0];
		
		/* Xively Feed ID */
		EELoadData(cnt_ee_i, ee_i_d, 1, EE_BYTE);
		EELoadString(cnt_ee_d, XivelyFeedID, ee_i_d[0]);
		XivelyFeedID[(int)ee_i_d[0]]='\0';
		cnt_ee_i=0;
		cnt_ee_d=2;
		
		sprintf(cusHeader, "X-APIKey: %s\r\nContent-Type: application/x-www-form-urlencoded\r\n", XivelyAPIKey);
		sprintf(cusPath, "/v2/feeds/%s", XivelyFeedID);

		IOPut(LED1_Pin,1);
	}
	state = IDLE;
	
    while(1)
    {
		// routine to saving new network parameters
		if (ParamSet)
		{
			_dbgwrite("Params changed!\r\n");
			WFDisconnect();
			while (WFStatus != NOT_CONNECTED);
			vTaskDelay(50);
			WFCustomSave();
			_dbgwrite("Custom config saved!\r\n");
			_dbgwrite("Connecting custom...\r\n");
			WFConnect(WF_CUSTOM);
			while (WFStatus != CONNECTED);
			sprintf(cusHeader, "X-APIKey: %s\r\nContent-Type: application/x-www-form-urlencoded\r\n", XivelyAPIKey);
			sprintf(cusPath, "/v2/feeds/%s", XivelyFeedID);
			
			/* Xively API Key */
			len = strlen(XivelyAPIKey);
			EESaveString(cnt_ee_d, XivelyAPIKey, len);
			cnt_ee_d += len;
			ee_i_d[0] = len;
			EESaveData(cnt_ee_i, ee_i_d, 1, EE_BYTE);
			cnt_ee_i++;
			
			/* Xively Feed ID */
			len = strlen(XivelyFeedID);
			EESaveString(cnt_ee_d, XivelyFeedID, len);
			cnt_ee_d += len;
			ee_i_d[0] = len;
			EESaveData(cnt_ee_i, ee_i_d, 1, EE_BYTE);
			
			IOPut(LED1_Pin,1);		
			ParamSet = FALSE;
		}
		
		if(AppConfig.networkType==WF_INFRASTRUCTURE&&WFStatus == CONNECTED)
		{
			btt_time = 0;
			btt_time_0 = TickGetDiv64K();
			btt_time_1 = 0;
			
			/* button condition */
			while(get(button) && state==IDLE)
				btt_time_1 = TickGetDiv64K();

			/* button time */
			btt_time=btt_time_1-btt_time_0;	
			
			/* go to SET state */	
			if(btt_time>1&&btt_time<5)
				state = SET;
			//if(state==TRIGGERED && get(button))
			//	state = STOP;

			/* go to TRIGGER state, the alarm is triggered */
			if(((float)time-(float)starttime)>seconds&&state==ACTIVE)
				state = TRIGGER;

			/* state machine sniffing routine */
			state_machine();
		}
	}
}

/* state machine of the system/alarm */
void state_machine()
{
	switch(state)
	{
		case SET:
			blink(2);
			distance = get(ranger);
			while(!(distance>20&&!readError()))
				distance = get(ranger);
			while(!readError()&&state == SET)
			{
				distance_zero = distance; //threshold
				starttime = TickGetDiv64K();
				XivelyClient = TCPClientOpen(XivelyServer, XivelyPort);
				timeout = 0;
				//	Waiting for timeout
				while(!TCPisConn(XivelyClient))
				{
					vTaskDelay(20);
					_dbgwrite(".");
					timeout++;
					if(timeout >= 25)
						break;
				}
				if(timeout >= 25)
				{
					TCPClientClose(XivelyClient);
					_dbgwrite("ERROR\r\n");
				}
				else
				{
					sprintf(myData, "{\"version\":\"1.0.0\",\"datastreams\":[{\"id\":\"Alarm\",\"current_value\":\"0\"}]}");
					resp_code = HTTP_Put(XivelyClient, XivelyServer, cusPath, cusHeader, myData, resp_Header, 100, resp_Body, 100, 700);						
					if(resp_code == 200)
					{
						_dbgwrite("HTTP request OK\r\n");
						state = ACTIVE;
					}
					else
						_dbgwrite("HTTP request ERROR\r\n");
					TCPClientClose(XivelyClient);

				}
			}
		break;
		case ACTIVE:
			distance = get(ranger);
			if(!readError())
			{
				if(distance<(distance_zero+40))
					time=TickGetDiv64K();
				else
				{
					time = 0;
					starttime = TickGetDiv64K();
				}
			}
		break;
		case TRIGGER:
			blink(3);
			XivelyClient = TCPClientOpen(XivelyServer, XivelyPort);
			timeout = 0;
			//	Waiting for timeout
			while(!TCPisConn(XivelyClient))
			{
				vTaskDelay(20);
				_dbgwrite(".");
				timeout++;
				if(timeout >= 25)
					break;
			}
			if(timeout >= 25)
			{
				TCPClientClose(XivelyClient);
				_dbgwrite("ERROR\r\n");
			}
			else
			{
				sprintf(myData, "{\"version\":\"1.0.0\",\"datastreams\":[{\"id\":\"Alarm\",\"current_value\":\"0\"}]}");
				resp_code = HTTP_Put(XivelyClient, XivelyServer, cusPath, cusHeader, myData, resp_Header, 100, resp_Body, 100, 700);						
				if(resp_code == 200)
				{
					_dbgwrite("HTTP request OK\r\n");
					state = TRIGGERED;
				}
				else
					_dbgwrite("HTTP request ERROR\r\n");
				TCPClientClose(XivelyClient);
			}
			vTaskDelay(50);
			XivelyClient = TCPClientOpen(XivelyServer, XivelyPort);
			timeout = 0;
			//	Waiting for timeout
			while(!TCPisConn(XivelyClient))
			{
				vTaskDelay(20);
				_dbgwrite(".");
				timeout++;
				if(timeout >= 25)
					break;
			}
			if(timeout >= 25)
			{
				TCPClientClose(XivelyClient);
				_dbgwrite("ERROR\r\n");
			}
			else
			{
				sprintf(myData, "{\"version\":\"1.0.0\",\"datastreams\":[{\"id\":\"Alarm\",\"current_value\":\"1\"}]}");
				resp_code = HTTP_Put(XivelyClient, XivelyServer, cusPath, cusHeader, myData, resp_Header, 100, resp_Body, 100, 700);						
				if(resp_code == 200)
				{
					_dbgwrite("HTTP request OK\r\n");
					state = TRIGGERED;
				}
				else
					_dbgwrite("HTTP request ERROR\r\n");
				TCPClientClose(XivelyClient);
			}
		break;
		case TRIGGERED:
			PWMInit(1,2,50);
			PWMOn(LED2_Pin,1);
			starttime=TickGetDiv64K();
			time = 0;
			while(!(((float)time-(float)starttime)>afterseconds && ((float)time-(float)starttime)>0))
			{
				distance = get(ranger);
				/*if(!readError())
				{*/
					if(distance>(distance_zero+40)||distance==-1)
						time=TickGetDiv64K();
					else
					{
						time = 0;
						starttime=TickGetDiv64K();
					}
				//}
				sprintf(dbg_string,"\r\n%2.1f$%2.1f$%2.1f$\r\n",(double)time,(double)starttime, (double) distance);
				_dbgwrite(dbg_string);
			}
			XivelyClient = TCPClientOpen(XivelyServer, XivelyPort);
			timeout = 0;
			//	Waiting for timeout
			while(!TCPisConn(XivelyClient))
			{
				vTaskDelay(20);
				_dbgwrite(".");
				timeout++;
				if(timeout >= 25)
					break;
			}
			if(timeout >= 25)
			{
				TCPClientClose(XivelyClient);
				_dbgwrite("ERROR\r\n");
			}
			else
			{
				sprintf(myData, "{\"version\":\"1.0.0\",\"datastreams\":[{\"id\":\"Alarm\",\"current_value\":\"1\"}]}");
				resp_code = HTTP_Put(XivelyClient, XivelyServer, cusPath, cusHeader, myData, resp_Header, 100, resp_Body, 100, 700);						
				if(resp_code == 200)
				{
					_dbgwrite("HTTP request OK\r\n");
					state = TRIGGERED;
				}
				else
					_dbgwrite("HTTP request ERROR\r\n");
				TCPClientClose(XivelyClient);
			}
			vTaskDelay(50);
			XivelyClient = TCPClientOpen(XivelyServer, XivelyPort);
			timeout = 0;
			//	Waiting for timeout
			while(!TCPisConn(XivelyClient))
			{
				vTaskDelay(20);
				_dbgwrite(".");
				timeout++;
				if(timeout >= 25)
					break;
			}
			if(timeout >= 25)
			{
				TCPClientClose(XivelyClient);
				_dbgwrite("ERROR\r\n");
			}
			else
			{
				sprintf(myData, "{\"version\":\"1.0.0\",\"datastreams\":[{\"id\":\"Alarm\",\"current_value\":\"0\"}]}");
				resp_code = HTTP_Put(XivelyClient, XivelyServer, cusPath, cusHeader, myData, resp_Header, 100, resp_Body, 100, 700);						
				if(resp_code == 200)
				{
					_dbgwrite("HTTP request OK\r\n");
					state = TRIGGERED;
				}
				else
					_dbgwrite("HTTP request ERROR\r\n");
				TCPClientClose(XivelyClient);
			}
			PWMOff(1);
			state = STOP;
		break;
		case STOP:
			state = IDLE;
		break;
		default:
		break;
	}
}	

/* blink routine */
void blink(int n)
{
	int i=0;
	for(i=0;i<n;i++)
	{
		IOPut(LED2_Pin, 1);
		vTaskDelay(25);
		IOPut(LED2_Pin, 0);
		vTaskDelay(25);
	}
}
