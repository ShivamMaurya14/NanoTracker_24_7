#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

#define SIM_RX 5   // D1
#define SIM_TX 4   // D2

const char* OWNER_NUMBER     = "+919999999999";
const char* LOGISTICS_NUMBER = "+918888888888";

SoftwareSerial sim800(SIM_RX, SIM_TX);

uint32_t sleepSeconds = 3600;

String readResponse(unsigned long timeout = 3000)
{
    String resp = "";
    unsigned long start = millis();

    while (millis() - start < timeout)
    {
        while (sim800.available())
        {
            resp += (char)sim800.read();
        }
    }
    return resp;
}

bool sendAT(String cmd, String expected = "OK", unsigned long timeout = 3000)
{
    while (sim800.available())
        sim800.read();

    sim800.println(cmd);

    String resp = readResponse(timeout);

    Serial.println("CMD : " + cmd);
    Serial.println(resp);

    return resp.indexOf(expected) != -1;
}

bool initSIM800()
{
    for(int i=0;i<5;i++)
    {
        if(sendAT("AT"))
            return true;

        delay(1000);
    }

    return false;
}

bool waitForNetwork()
{
    for(int i=0;i<20;i++)
    {
        sim800.println("AT+CREG?");
        String r = readResponse(2000);

        if(r.indexOf(",1") != -1 || r.indexOf(",5") != -1)
            return true;

        delay(1000);
    }

    return false;
}

String getOperatorInfo()
{
    sim800.println("AT+COPS?");
    return readResponse(3000);
}

String getCellInfo()
{
    sendAT("AT+CREG=2");

    sim800.println("AT+CREG?");
    String resp = readResponse(3000);

    String msg = "Cell Info\n";
    msg += resp;

    return msg;
}

float readBatteryVoltage()
{
    int raw = analogRead(A0);

    float adcVoltage = (raw / 1023.0f);

    // Reference approximation for your divider.
    float batteryVoltage = adcVoltage * 4.2f;

    return batteryVoltage;
}

bool sendSMS(String number, String text)
{
    sim800.println("AT+CMGF=1");

    if(readResponse(2000).indexOf("OK") == -1)
        return false;

    sim800.print("AT+CMGS=\"");
    sim800.print(number);
    sim800.println("\"");

    String prompt = readResponse(3000);

    if(prompt.indexOf(">") == -1)
        return false;

    sim800.print(text);

    sim800.write(26);

    String result = readResponse(15000);

    return result.indexOf("+CMGS:") != -1;
}

void saveSleep()
{
    EEPROM.put(0, sleepSeconds);
    EEPROM.commit();
}

void loadSleep()
{
    EEPROM.get(0, sleepSeconds);

    if(sleepSeconds < 60 || sleepSeconds > 86400)
        sleepSeconds = 3600;
}

void processCommand(String cmd)
{
    cmd.trim();

    if(cmd.indexOf("MODE:TRACK") != -1)
    {
        sleepSeconds = 60;
        saveSleep();
    }
    else if(cmd.indexOf("MODE:SAVE") != -1)
    {
        sleepSeconds = 3600;
        saveSleep();
    }
    else if(cmd.startsWith("SLEEP:"))
    {
        uint32_t value = cmd.substring(6).toInt();

        if(value >= 60 && value <= 86400)
        {
            sleepSeconds = value;
            saveSleep();
        }
    }
}

void checkCommands()
{
    sim800.println("AT+CMGL=\"REC UNREAD\"");

    String resp = readResponse(5000);

    if(resp.indexOf(LOGISTICS_NUMBER) == -1)
        return;

    if(resp.indexOf("MODE:TRACK") != -1)
        processCommand("MODE:TRACK");

    if(resp.indexOf("MODE:SAVE") != -1)
        processCommand("MODE:SAVE");

    int p = resp.indexOf("SLEEP:");
    if(p != -1)
    {
        int e = resp.indexOf("\n", p);
        processCommand(resp.substring(p, e));
    }

    sim800.println("AT+CMGD=1,4");
    readResponse(3000);
}

void goSleep()
{
    sendAT("AT+CSCLK=2");

    Serial.println("Sleeping...");
    Serial.flush();

    ESP.deepSleep((uint64_t)sleepSeconds * 1000000ULL);
}

void setup()
{
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();

    Serial.begin(115200);

    sim800.begin(9600);

    EEPROM.begin(64);

    loadSleep();

    delay(2000);

    if(!initSIM800())
    {
        goSleep();
    }

    if(!waitForNetwork())
    {
        goSleep();
    }

    String report = "";
    report += "Tracker Status\n\n";

    report += getCellInfo();
    report += "\n";

    float batt = readBatteryVoltage();

    report += "Battery: ";
    report += String(batt, 2);
    report += "V\n";

    report += "Sleep: ";
    report += String(sleepSeconds);
    report += " sec\n";

    sendSMS(OWNER_NUMBER, report);
    sendSMS(LOGISTICS_NUMBER, report);

    checkCommands();

    goSleep();
}

void loop()
{
}

