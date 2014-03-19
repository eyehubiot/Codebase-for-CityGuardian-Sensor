#include <Time.h>
//#include <EEPROM.h>
#include <SPI.h>
#include <HttpClient.h>
#include <GSM.h>

// APN data
#define GPRS_APN       "eseye.com" // replace your GPRS APN
#define GPRS_LOGIN     "User"    // replace with your GPRS login
#define GPRS_PASSWORD  "Pass" // replace with your GPRS password

#define EYEHUB_DEVICE_MANAGER_ID  "CityGuardian"
#define EYEHUB_DEVICE_ID  "NoiseMonitorTest"
#define EYEHUB_USERNAME  "USERNAME"
#define EYEHUB_PASSWORD  "PASSWORD"

/* Pin definitions*/
#define SPEC_STROBE 4   // Spectrun Analyser strobe Pin
#define SPEC_RESET 5    // Spectrun Analyser Reset Pin
#define SPEC_ANALOGUE 3    // Spectrun Analyser Analogue Pin

#define kHostname "config28.flexeye.com"	// Name of the server we want to connect to  

#define kNetworkTimeout 30000	// Number of milliseconds to wait without receiving any data before we give up
#define kNetworkDelay 1000	// Number of milliseconds to wait if no data is available before trying again

#define IMEI "YOURIMEI"

const byte MeasurementFrequncy = 10;	//Noise Measurement Frequency in Minutes, 10 mins default value
const byte ReportingFrequency = 59;	//Frequency to report to EyeHub in minutes, 10 min default value
unsigned int AlertLimits[7] = {900, 900, 900, 900, 900, 900, 900};
byte SpecMemLoc = 0;	//Spectrum Memory location
const byte SpecMemLocSize = 6;	//Size of the spectrum memory
unsigned int Spectrum[int(SpecMemLocSize)][7];	//Spectrum memory
char Time[SpecMemLocSize][25];	//Time memory
byte LastReading;	//Time of last reading
bool EndOfHourDontRead = false;
byte LastReport;	//Time of last report to Eyehub
bool EndOfHourDontReport = false;

// initialize the library instance
GSM3ShieldV1AccessProvider gsmAccess;     // include a 'true' parameter to enable debugging
GSM3ShieldV1DataNetworkProvider gprs;

const bool Debug = false;
byte DisregardReadings = 3;

void setup() {
	Serial.begin(9600);
	if (Debug)	{
		while (!Serial) {
			// wait for serial port to connect. Needed for Leonardo only 
		}
	}
	
	//Turn off LED to save power
	pinMode(13, OUTPUT);
	digitalWrite(13, HIGH);
	
	InitialiseSpectrumAnalyser();
	ReportBoot();
	if (Debug)	{
		Serial.print(F("Free Memory: "));
		Serial.println(freeMemory()); 
	}
	ClearSpecMem();
}

void loop() {
	bool SendData = false;
	byte Band;
	
	if (EndOfHourDontRead) {
		if ( (60 - minute()) >= MeasurementFrequncy ) {	//
			EndOfHourDontRead = false;
		}
	}
	if (EndOfHourDontReport) {
		if ( (60 - minute()) >= ReportingFrequency ) {
			EndOfHourDontReport = false;
		}
	}
	//Take a reading every 10 seconds
	if (second() % 10==0) {
		if (DisregardReadings == 0 ) {
			TakeSpectrumReading();
		}
		else {
			ClearSpecMem();
			DisregardReadings--;
		}
	}
	//Check if time to time to complete reading interval
	if (minute() >= ( LastReading + MeasurementFrequncy ) && ( ! EndOfHourDontRead ) ) {
		//Record the time
		sprintf(Time[SpecMemLoc], "%d-%02d-%02dT%02d:%02d:%02d.%03dZ", year(), month(), day(), hour(), minute(), second(), 000);
		if (Debug)	{
			Serial.print (F("TimeStamp - "));
			Serial.println (Time[SpecMemLoc]);
		}
		//Check if outside alert limits
		for(Band=0;Band <7; Band++) {
			if ( Spectrum[SpecMemLoc][Band] >= AlertLimits[Band] ) {
				if (Debug)	{
					Serial.print(F("Data outside alert limits, band - "));
					Serial.println(Band);
				}
				SendData = true;
			}
		}
		//Increment Spectrum Memory Location ready for next reading
		SpecMemLoc++;
		//Set reading time to now
		LastReading = minute();
		if ( ( LastReading + MeasurementFrequncy ) > 59 ) {	//Account for minute roll-over
			Serial.print(F("Accounting for minute roll-over - "));
			Serial.print(LastReading);
			Serial.print(F(" - "));
			Serial.print(MeasurementFrequncy);
			Serial.print(F(" - "));
			Serial.println(( LastReading + MeasurementFrequncy ));
			LastReading = (( LastReading + MeasurementFrequncy ) - 60);
			Serial.print(F("Changed to - "));
			Serial.println(LastReading);
			EndOfHourDontRead = true;
		}
	}
	//Check if time to report
	if (minute() >= ( LastReport + ReportingFrequency ) && ( ! EndOfHourDontReport ) ) {
		Serial.println("Time to report");
		SendData = true;
	}
	//Check if SpecMemLoc more than memory size
	if (SpecMemLoc >= SpecMemLocSize) {
		Serial.print(F("SpecMemLoc more than memory size"));
		SendData = true;
	}
	//See if time to send data
	if (SendData) {
		ConnectToNetwork();
		while (SpecMemLoc) {
			byte CurrSpecMemLoc = (SpecMemLoc - 1); //SpecMemLoc is always one ahead of last value at this point.
			Serial.print("Reporting Value: ");
			Serial.println(CurrSpecMemLoc);
			char Payload[35];
			sprintf(Payload, "%i,%i,%i,%i,%i,%i,%i", Spectrum[CurrSpecMemLoc][0], Spectrum[CurrSpecMemLoc][1], Spectrum[CurrSpecMemLoc][2], Spectrum[CurrSpecMemLoc][3], Spectrum[CurrSpecMemLoc][4], Spectrum[CurrSpecMemLoc][5], Spectrum[CurrSpecMemLoc][6]);
			Serial.print(F("Sending Data: "));
			Serial.println(Payload);
			ReportToEyeHub("MSGEQ7", IMEI, Payload, Time[CurrSpecMemLoc]);
			
			//Data sent zero memory ready for next readings
			for(Band=0;Band <7; Band++) {
				Spectrum[CurrSpecMemLoc][Band] = 0;
			}
			SpecMemLoc--;
		}
		ReportToEyeHub("TimeSync", IMEI, "1", 0);
		DisconnectFromNetwork();
		Serial.print(F("Ended Reporting: "));
		Serial.println(SpecMemLoc);
		ClearSpecMem();
		DisregardReadings = 3;
		LastReport = minute();
		if ( ( LastReport + ReportingFrequency ) > 59 ) {	//Account for minute rollover
			Serial.print(F("Accounting for minute roll-over - "));
			Serial.println(LastReport);
			LastReport = (( LastReport + ReportingFrequency ) - 60);
			Serial.print(F("Changed to - "));
			Serial.println(LastReport);
			EndOfHourDontReport = true;
		}
	}
	PrintTime();
	delay (1000);
}

void PrintTime()	{
	Serial.print(hour());
	Serial.print(F(":"));
	Serial.print(minute());
	Serial.print(F(":"));
	Serial.print(second());
	Serial.print(F(" "));
	Serial.print(day());
	Serial.print(F("/"));
	Serial.print(month());
	Serial.print(F("/"));
	Serial.println(year());
}

void GetIMEIFromModem()	{
	//Serial.print("Starting modem test...");
	//GSMModemlite modem;
	//modem.begin();
	
	//Serial.print("Getting IMEI...");
	//String TmpIMEI = "";
	//TmpIMEI = modem.getIMEI();
	//Serial.println(TmpIMEI);
	//char IMEI[18];
	//TmpIMEI.toCharArray(IMEI, 17);
  
    //Serial.print("Modem's IMEI: ");
	//Serial.println(IMEI);
	//return IMEI;
}

void ReportBoot() {
	//Inform EyeHub of boot event 
	bool SearchingForTime = true;
	while (SearchingForTime) {
		ConnectToNetwork();
		if (ReportToEyeHub("SystemBootEvent", IMEI, "1", 0) ) {
			SearchingForTime = false;
		}
		Serial.println(F("Finished fetch"));
		DisconnectFromNetwork();
	}	
	LastReading = minute();
	if ( ( LastReading + MeasurementFrequncy ) > 59 ) {	//Account for minute rollover
		Serial.print(F("Accounting for minute roll-over - "));
		Serial.println(LastReading);
		LastReading = (( LastReading + MeasurementFrequncy ) - 60);
		Serial.print(F("Changed to - "));
		Serial.println(LastReading);
		EndOfHourDontRead = true;
	}
	//LastReport = minute();
	LastReport = 30;
	if ( ( LastReport + MeasurementFrequncy ) > 59 ) {	//Account for minute rollover
		Serial.print(F("Accounting for minute roll-over - "));
		Serial.println(LastReport);
		LastReport = (( LastReport + ReportingFrequency ) - 60);
		Serial.print(F("Changed to - "));
		Serial.println(LastReport);
		EndOfHourDontReport = true;
	}
}

void TakeSpectrumReading()	{
	byte Band;
	
	// Check if fallen outside of memory range
	if (SpecMemLoc > SpecMemLocSize) {
		return;
	}
	for(Band=0;Band <7; Band++) {
		unsigned int ThisReading = (((Spectrum[SpecMemLoc][Band] * 10) + analogRead(SPEC_ANALOGUE)) / 11);
		Serial.print (Band);
		Serial.print (F(" - "));
		Serial.print (ThisReading);
		//Serial.print (F(" - Max - "));
		//Serial.println (Spectrum[SpecMemLoc][Band]);
		//if ( ThisReading > Spectrum[SpecMemLoc][Band] ) {
		//	Serial.print (F("Replacing "));
		//	Serial.print (Spectrum[SpecMemLoc][Band]);
		//	Serial.print (F("With "));
		//	Serial.println (ThisReading);
			Spectrum[SpecMemLoc][Band] = ThisReading;
		//}		
		digitalWrite(SPEC_STROBE,HIGH);  //Strobe pin on the shield
		delay(1);
		digitalWrite(SPEC_STROBE,LOW);     
		delay(1);
	}
	//Clear spectrum shield buffer, degrades 10% each time so do 10 readings to ensure it is completly wiped out.
	byte Wipe;
	for(Wipe=0;Wipe < 11; Wipe++) {
		for(Band=0;Band < 7; Band++) {
			digitalWrite(SPEC_STROBE,HIGH);  //Strobe pin on the shield
			delay(1);
			digitalWrite(SPEC_STROBE,LOW);     
			delay(1);
		}
	}
	//SpecMemLoc++;
}

void ClearSpecMem() {
	//Clear spectrum shield buffer, degrades 10% each time so do 10 readings to ensure it is completly wiped out.
	byte Wipe;
	byte Band;
	for(Wipe=0;Wipe < 11; Wipe++) {
		for(Band=0;Band < 7; Band++) {
			digitalWrite(SPEC_STROBE,HIGH);  //Strobe pin on the shield
			delay(1);
			digitalWrite(SPEC_STROBE,LOW);     
			delay(1);
		}
	}
}

/*void  CheckMemoryForIntervals() {
	//check if intervals are stored in non volitile memory, if not use defaults
	unsigned int VolatileMemStore;
	if ( VolatileMemStore = EEPROM.read(MeasurementFrequncyAddress) ) {
		if (VolatileMemStore != 255) {
			//Serial.print ("Measurement Frequncy value found changing to - ");
			//Serial.println (VolatileMemStore);
			MeasurementFrequncy = VolatileMemStore;
		}
		else {
			//Serial.println ("No Measurement Frequncy value found, leaving as default value");
		}
	}
	if ( VolatileMemStore = EEPROM.read(ReadsPerMeasurementAddress) ) {
		if (VolatileMemStore != 255) {
			//Serial.print ("ReadsPerMeasurement value found changing to - ");
			//Serial.println (VolatileMemStore);
			ReadsPerMeasurement = VolatileMemStore;
		}
		else {
			//Serial.println ("No ReadsPerMeasurement value found, leaving as default value");
		}
	}
	if ( VolatileMemStore = EEPROM.read(ReportingFrequncyAddress) ) {
		if (VolatileMemStore != 255) {
			//Serial.print ("ReportingFrequncy value found changing to - ");
			//Serial.println (VolatileMemStore);
			ReportingFrequncy = VolatileMemStore;
		}
		else {
			//Serial.println ("No ReportingFrequncy value found, leaving as default value");
		}
	}
	byte Band;
	for(Band=0;Band <7; Band++) {
		if ( VolatileMemStore = EEPROM.read(AlertLimitsAddresses[Band]) ) {
			if (VolatileMemStore != 255) {
				//Serial.print ("AlertLimits value found changing to - ");
				//Serial.println (VolatileMemStore);
				AlertLimits[Band] = VolatileMemStore;
			}
			else {
				//Serial.println ("No AlertLimits value found, leaving as default value");
			}
		}
	}
}
*/

void InitialiseSpectrumAnalyser ()	{
	//Initialise Spectrum shield
	//Setup pins to drive the spectrum analyzer. It needs RESET and STROBE pins.
	pinMode(SPEC_RESET, OUTPUT);
	pinMode(SPEC_STROBE, OUTPUT);
	//Init spectrum analyzer
	digitalWrite(SPEC_STROBE,LOW);
	digitalWrite(SPEC_RESET,HIGH);
	digitalWrite(SPEC_STROBE,HIGH);
	digitalWrite(SPEC_STROBE,LOW);
	digitalWrite(SPEC_RESET,LOW);
}

int ReportToEyeHub(char* SourceDesc, char* DeviceID, char* PayloadValue, char* TimeStamp) {
	//Serial.println("Connected");
	bool TimeSet = false;
//Serial.println(freeMemory());	
	// Path to download (this is the bit after the hostname in the URL
	// that you want to download
	int pathlength = 1;	//Initially 1 to account for the ending \0
	pathlength += strlen("/v1/iot_Default/dms/" EYEHUB_DEVICE_MANAGER_ID "/devices//events");
	pathlength += strlen(DeviceID);
	char kPath[pathlength];
	sprintf(kPath, "/v1/iot_Default/dms/" EYEHUB_DEVICE_MANAGER_ID "/devices/%s/events", DeviceID);
//Serial.println(freeMemory());	
	GSMClient c;
	HttpClient http(c);
	
	http.beginRequest();
	int err = 0;
	//delay(200);
	//gsmAccess = 1;
	err = http.post(kHostname, kPath);
	//gsmAccess = 0;
	//Serial.println("Sent post");
	if (err == 0) {
		//Serial.println("startedRequest ok");
		// Send our login credentials
		http.sendBasicAuth(EYEHUB_USERNAME, EYEHUB_PASSWORD);
		// Tell the server that we'll be sending some JSON data over
		http.sendHeader("Content-Type", "application/json");
		
		// Work out how much data we're going to send
		// THIS NEEDS TO EXACTLY MATCH THE STUFF WE SEND LATER ON
		unsigned int content_len = 0;
		if (TimeStamp) {
			content_len += strlen("{ \"type\":\"1.0\", \"source\":\"\", \"payload\":\"\", \"timeStamp\”:\"\"}");
			content_len += strlen(TimeStamp);
		}
		else {
			content_len += strlen("{ \"type\":\"1.0\", \"source\":\"\", \"payload\":\"\"}");
		}
		content_len += strlen(SourceDesc);
		content_len += strlen(PayloadValue);
	//Serial.println(freeMemory());	
		// And tell the server
		http.sendHeader("Content-Length", content_len);
	//Serial.println(freeMemory());
		// We've finished sending the headers now
		http.endRequest();
	//Serial.println(freeMemory());
		// Now send the data over
		// THIS NEEDS TO EXACTLY MATCH THE STUFF WE CALCULATED THE LENGTH OF ABOVE
		http.print(F("{ "));
		http.print(F("\"type\":\"1.0\", "));
		http.print(F("\"source\":\""));
		http.print(SourceDesc);
		http.print(F("\", "));
		http.print(F("\"payload\":\""));
		http.print(PayloadValue);
		http.print(F("\""));
		if (TimeStamp) {
			http.print(F(", "));
			http.print(F("\"timeStamp\":\""));
			http.print(TimeStamp);
			http.print(F("\""));
		}
		http.print("}");
//Serial.println(freeMemory());
		err = http.responseStatusCode();
		if (err >= 0) {
			Serial.print(F("Got status code: "));
			Serial.println(err);
			
			// Check that the response code is 200 or a
			// similar "success" code (200-299) before carrying on
			// Return a negative value if failed.
			if ((err < 200) || (err > 299))	{
				// It wasn't a successful response, ensure it's -ve so the error is easy to spot
				http.flush();
				http.stop();
				return false;
			}
//Serial.println(freeMemory());	
			err = http.skipResponseHeaders();
			if (err >= 0) {
				unsigned int bodyLen = http.contentLength();
				//Serial.print("Content length is: ");
				//Serial.println(bodyLen);
				//Serial.println();
				//Serial.println("Body returned follows:");
				
				// Now we've got to the body, so we can print it out
				unsigned long timeoutStart = millis();
				char c;
				char CurrLine[50];  //Length is long enough for timestamp line.
				CurrLine[49] = '\0';
				//char TimeStamp[] = "timeStamp";
				unsigned int CharInLineCount = 0;
				// Whilst we haven't timed out & haven't reached the end of the body
	//Serial.println(freeMemory());	
				while ( (http.connected() || http.available()) && ((millis() - timeoutStart) < kNetworkTimeout) ) {
					if (http.available()) {
						c = http.read();
						if (! TimeStamp) {	//Only look for timestamp if not provided by me
							//Look for "timeStamp" : "
							if (c == '\n') {
								CurrLine[CharInLineCount] = '\0';
								char *TimeStrStart;
								char *TimeStrEnd;
								if ((TimeStrStart = strstr(CurrLine, "\"timeStamp\" : \"")) != NULL && (TimeStrEnd = strstr(CurrLine, "Z\",")) != NULL) {
									*TimeStrEnd = '\0';	//end string at end of the time
									TimeStrStart += strlen("\"timeStamp\" : \"");
									char Year[5];
									strncpy(Year, TimeStrStart, 4);
									Year[4] = '\0';
									char Month[3];
									strncpy(Month, TimeStrStart + 5, 2);
									Month[2] = '\0';
									char Day[3];
									strncpy(Day, TimeStrStart + 8, 2);
									Day[2] = '\0';
									char Hour[3];
									strncpy(Hour, TimeStrStart + 11, 2);
									Hour[2] = '\0';
									char Mins[3];
									strncpy(Mins, TimeStrStart + 14, 2);
									Mins[2] = '\0';
									char Secs[3];
									strncpy(Secs, TimeStrStart + 17, 2);
									Secs[2] = '\0';
									Serial.println("Sucesfully sent data and found Timestamp");
									//Use time from EyeHub return to start or update clock.
									setTime(atoi(Hour),atoi(Mins),atoi(Secs),atoi(Day),atoi(Month),atoi(Year));
									TimeSet = true;
								}
								CharInLineCount = 0;
							}
							else {
								if (CharInLineCount < 49) {
									CurrLine[CharInLineCount] = c;
								}
								CharInLineCount++;
							}
						}
						bodyLen--;
						// We read something, reset the timeout counter
						timeoutStart = millis();
					}
					else {
						// We haven't got any data, so let's pause to allow some to arrive
						delay(kNetworkDelay);
					}
				}
			}
			else {
				Serial.print(F("Failed to skip response headers: "));
				Serial.println(err);
				return false;
			}
		}
		else {    
			Serial.print(F("Getting response failed: "));
			Serial.println(err);
			return false;
		}
	}
	else {
		Serial.print(F("Connect failed: "));
		Serial.println(err);
		return false;
	}
	http.flush();
	http.stop();
	return TimeSet;
}

int DoHTTPRequest(char* HostName, char* Path, byte RequestType, char* Body, char* PatternStart, char* PatternEnd) {
	//RequestType 1 = GET
	//RequestType 2 = POST
	
	int pathlength = 1;	//Initially 1 to account for the ending \0
	pathlength += strlen(Path);
	
	GSMClient c;
	HttpClient http(c);
	
	http.beginRequest();
	int Response = 0;
	Response = http.get(HostName, Path);
	if (Response == 0) {
		http.sendBasicAuth(EYEHUB_USERNAME, EYEHUB_PASSWORD);
		//http.sendHeader("Content-Type", "application/json");
		
		//http.sendHeader("Content-Length", 0);

		http.endRequest();

		Response = http.responseStatusCode();
		if (Response >= 0) {
			if ((Response < 200) || (Response > 299))	{
				// It wasn't a successful response, ensure it's -ve so the error is easy to spot
				if (Response > 0)	{
					Response = Response * -1;
				}
				http.flush();
				http.stop();
				return Response;
			}

			Response = http.skipResponseHeaders();
			if (Response >= 0) {
				unsigned int bodyLen = http.contentLength();
				unsigned long timeoutStart = millis();
				char c;
				char CurrLine[100];  //Length is long enough for timestamp line.
				CurrLine[99] = '\0';
				unsigned int CharInLineCount = 0;
				// Whilst we haven't timed out & haven't reached the end of the body
				while ( (http.connected() || http.available()) && ((millis() - timeoutStart) < kNetworkTimeout) ) {
					if (http.available()) {
						c = http.read();
						//Look for "uri" : "
						if (c == '\n') {
							CurrLine[CharInLineCount] = '\0';
							char *URIStart;
							char *URIEnd;
							if ((URIStart = strstr(CurrLine, "\"uri\" : \"")) != NULL && (URIEnd = strstr(CurrLine, "\",")) != NULL) {
								*URIEnd = '\0';	//end string at end of the URI
								URIStart += strlen("\"uri\" : \"");	//Disregard initial part of string.
								unsigned int URILength = 1;
								URILength += strlen(URIStart);
								char URI[URILength];
								strncpy(URI, URIStart, URILength);
								URI[URILength] = '\0';
								Serial.print(F("Found URI: "));
								Serial.println(URI);
							}
							CharInLineCount = 0;
						}
						else {
							if (CharInLineCount < 99) {
								CurrLine[CharInLineCount] = c;
							}
							CharInLineCount++;
						}
						
						bodyLen--;
						// We read something, reset the timeout counter
						timeoutStart = millis();
					}
					else {
						// We haven't got any data, so let's pause to allow some to arrive
						delay(kNetworkDelay);
					}
				}
			}
			else {
				Serial.print(F("Failed to skip response headers: "));
				Serial.println(Response);
				return 0;
			}
		}
		else {    
			Serial.print(F("Getting response failed: "));
			Serial.println(Response);
			return 0;
		}
	}
	else {
		Serial.print(F("Connect failed: "));
		Serial.println(Response);
		return 0;
	}
	Serial.print(F("Finished fetching from Eyehub"));
	http.flush();
	http.stop();
	return 1;
}

/*int FetchValueFromEyeHub(char* DeviceID, char* SourceDesc) {
	Serial.print("Fetching from Eyehub");
	//const char kHostname[] = "config28.flexeye.com";
	int pathlength = 1;	//Initially 1 to account for the ending \0
	pathlength += strlen("/v1/iot_Default/dms/" EYEHUB_DEVICE_MANAGER_ID "/devices//events");
	pathlength += strlen(DeviceID);
	char kPath[pathlength];
	sprintf(kPath, "/v1/iot_Default/dms/" EYEHUB_DEVICE_MANAGER_ID "/devices/%s/events", DeviceID);
	
	GSMClient c;
	HttpClient http(c);
	
	http.beginRequest();
	int err = 0;
	err = http.get(kHostname, kPath);
	if (err == 0) {
		http.sendBasicAuth(EYEHUB_USERNAME, EYEHUB_PASSWORD);
		//http.sendHeader("Content-Type", "application/json");
		
		//http.sendHeader("Content-Length", 0);

		http.endRequest();

		err = http.responseStatusCode();
		if (err >= 0) {
			Serial.print("Got status code: ");
			Serial.println(err);
			if ((err < 200) || (err > 299))	{
				// It wasn't a successful response, ensure it's -ve so the error is easy to spot
				if (err > 0)	{
					err = err * -1;
				}
				http.flush();
				http.stop();
				return err;
			}

			err = http.skipResponseHeaders();
			if (err >= 0) {
				unsigned int bodyLen = http.contentLength();
				unsigned long timeoutStart = millis();
				char c;
				char CurrLine[100];  //Length is long enough for timestamp line.
				CurrLine[99] = '\0';
				unsigned int CharInLineCount = 0;
				// Whilst we haven't timed out & haven't reached the end of the body
				while ( (http.connected() || http.available()) && ((millis() - timeoutStart) < kNetworkTimeout) ) {
					//Serial.println("Stuck in loop");
					if (http.available()) {
						c = http.read();
						//Look for "uri" : "
						if (c == '\n') {
							CurrLine[CharInLineCount] = '\0';
							char *URIStart;
							char *URIEnd;
							if ((URIStart = strstr(CurrLine, "\"uri\" : \"")) != NULL && (URIEnd = strstr(CurrLine, "\",")) != NULL) {
								*URIEnd = '\0';	//end string at end of the URI
								URIStart += strlen("\"uri\" : \"");	//Disregard initial part of string.
								unsigned int URILength = 1;
								URILength += strlen(URIStart);
								char URI[URILength];
								strncpy(URI, URIStart, URILength);
								URI[URILength] = '\0';
								Serial.print("Found URI: ");
								Serial.println(URI);
							}
							CharInLineCount = 0;
						}
						else {
							if (CharInLineCount < 99) {
								CurrLine[CharInLineCount] = c;
							}
							CharInLineCount++;
						}
						
						bodyLen--;
						// We read something, reset the timeout counter
						timeoutStart = millis();
					}
					else {
						// We haven't got any data, so let's pause to allow some to arrive
						delay(kNetworkDelay);
					}
				}
				Serial.println("Left loop");
				Serial.println(freeMemory());
			}
			else {
				Serial.print("Failed to skip response headers: ");
				Serial.println(err);
				return 0;
			}
		}
		else {    
			Serial.print("Getting response failed: ");
			Serial.println(err);
			return 0;
		}
	}
	else {
		Serial.print("Connect failed: ");
		Serial.println(err);
		return 0;
	}
	Serial.println("flushing");
	http.flush();
	Serial.println("Stopping");
	http.stop();
	Serial.println("Stopped");
	return 1;
}*/

bool ConnectToNetwork() {
	Serial.println(F("Resetting Modem"));
	Serial.println(gsmAccess.HWrestart());
	Serial.println(F("Connecting to Network"));
	delay(1000);
	unsigned long timeoutStart = millis();
	
	// connection state
	boolean notConnected = true;
	unsigned int ConnectionAttempts = 0;
	// After starting the modem with GSM.begin()
	// attach the shield to the GPRS network with the APN, login and password
	while(notConnected && (ConnectionAttempts < 600)) {
		Serial.println("Connecting");
		if((gsmAccess.begin()==GSM_READY) && (gprs.attachGPRS(GPRS_APN, GPRS_LOGIN, GPRS_PASSWORD)==GPRS_READY)	) { //&& ((millis() - timeoutStart) < kNetworkTimeout)) {
			notConnected = false;
		}
		else {
			//Serial.println("Not connected");
			delay(kNetworkDelay);
			// We Waited a bit, reset the timeout counter before we try again
			timeoutStart = millis();
			ConnectionAttempts++;
		}
	}
	if (notConnected) {
		Serial.println(F("Connection failed"));
		return 0;
	}
	else {
		Serial.println(F("Sucessfully connected"));
		return 1;
	}
}

bool DisconnectFromNetwork() {
	Serial.println(F("Disconnecting from Network"));
	//Serial.println(gprs.detachGPRS());
	//while ( gprs.detachGPRS() != 1 ) {
	//	Serial.println(F("Disconnecting"));
	//	delay (1000);
	//}
	//Serial.println(F("Disconnected"));
	while ( gsmAccess.shutdown() != 1 ) {
		Serial.println(F("Shutting down"));
		delay (1000);
	}
	
	Serial.println(F("Sucessfully disconnected"));
	return 1;
}

int freeMemory() {
 extern int __heap_start, *__brkval; 
  int v; 
  return(int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}