// N20 encoder-controlled bench test (v7): the owner's bench sketch for the direct-drive disc (DRV8833 + GA12-N20 with
// encoder, Waveshare ESP32-S3-Zero), committed unchanged below this header as the reference the firmware's
// direct-drive mechanism (components/precision_feeder/mechanism_n20.*) was ported from. tests/test_n20.cpp runs it on
// a computer against a simulated motor (tests/motor_sim.h, tests/reference/Arduino.h) and checks that the firmware
// drives the bridge exactly as this does.
//
// To run it on the board itself (it is a standalone Arduino sketch, not part of the firmware): a PlatformIO project
// with platform espressif32@6.12.0, board esp32-s3-devkitc-1, framework arduino, flash 4MB (dio), build flags
// -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1, monitor at 115200 baud. Type h for its commands.
// Bench results: 15 forward 24-degree moves land on exactly 8344 ticks (one turn); 300 move-and-wiggle cycles over
// 20 turns ended within 5 ticks of the grid (docs/BENCH.md, plan docs/plans/drive-decoupling.md).
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense): the sketch is the project owner's,
// committed with their permission (2026-10-20). This header, and the tests and simulation around the sketch, were
// written with the assistance of an AI (Anthropic's Claude); reviewed by a human, but they may still contain mistakes.
#include <Arduino.h>
#include <math.h>
constexpr int IN1=1, IN2=2, ENABLE=6, ENC_A=4, ENC_B=5;
constexpr double TICKS_PER_REV=8344.0;
constexpr int32_t APPROACH_TICKS=160;
constexpr uint32_t INITIAL_BRAKE_MS=100, PULSE_BRAKE_MS=15;
constexpr uint32_t FINAL_BRAKE_MS=100, FINAL_SETTLE_MS=100;
volatile int32_t ticks=0;
volatile uint32_t edgesA=0, edgesB=0, invalid=0;
volatile uint8_t previous=0;
portMUX_TYPE encLock=portMUX_INITIALIZER_UNLOCKED;
const int8_t DRAM_ATTR transitions[16]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
void IRAM_ATTR encoder(){
  portENTER_CRITICAL_ISR(&encLock);
  uint8_t state=(digitalRead(ENC_A)<<1)|digitalRead(ENC_B), changed=state^previous;
  if(changed&2)edgesA++; if(changed&1)edgesB++; if(changed==3)invalid++;
  ticks+=transitions[(previous<<2)|state];previous=state;
  portEXIT_CRITICAL_ISR(&encLock);
}
int32_t count(){portENTER_CRITICAL(&encLock);int32_t t=ticks;portEXIT_CRITICAL(&encLock);return t;}
enum Phase {IDLE,MOVING,CREEP_WAIT,CREEP_DRIVE,BRAKING,SETTLING};
Phase phase=IDLE;
double plannedTicks=0.0;bool completedTarget=false;
int32_t startTicks=0,targetTicks=0,lastTicks=0;int direction=1;int requestedDuty=255;
uint32_t began=0,lastEdge=0,phaseSince=0,lastReport=0;
uint32_t disconnectedSince=0;bool disconnectStopPending=false;
uint32_t drivePulseMs=3;
uint32_t pulseMs=3; int32_t pulseStart=0; bool pulseDone=false;
// Wiggle state is separate from the feeding-position accumulator.
struct WiggleState {bool active=false;int32_t center=0;double savedPlan=0;int32_t amplitude=0;uint8_t cycles=0,leg=0;} wiggle;
bool startEncoderMove(int32_t target,int power=100);
void advanceWiggle();
char line[80];size_t used=0;bool discard=false;
void coast(){ledcWrite(0,0);ledcWrite(1,0);digitalWrite(ENABLE,LOW);}
void report(){
 portENTER_CRITICAL(&encLock);int32_t t=ticks;uint32_t a=edgesA,b=edgesB,e=invalid;portEXIT_CRITICAL(&encLock);
 Serial.printf("%s ticks=%ld degrees=%.3f A_edges=%lu B_edges=%lu invalid=%lu A=%d B=%d\n",phase==IDLE?"STOP":"BUSY",(long)t,t*360.0/TICKS_PER_REV,(unsigned long)a,(unsigned long)b,(unsigned long)e,digitalRead(ENC_A),digitalRead(ENC_B));
}
void brake(const char* reason,bool reached=false){
 completedTarget=reached;
 // Both inputs high = dynamic brake; release after a bounded settling interval.
 ledcWrite(0,255);ledcWrite(1,255);digitalWrite(ENABLE,HIGH);
 phase=BRAKING;phaseSince=millis();Serial.println(reason);
}
void finish(){
 phase=IDLE;int32_t t=count();
 if(!completedTarget){plannedTicks=t;if(wiggle.active){wiggle.active=false;Serial.println("WIGGLE ABORTED; plan rebased to actual.");}}
 Serial.printf("FINAL start=%ld target=%ld actual=%ld delta=%ld error=%ld ticks (%.3f deg)\n",(long)startTicks,(long)targetTicks,(long)t,(long)(t-startTicks),(long)(t-targetTicks),(t-targetTicks)*360.0/TICKS_PER_REV);report();
 if(wiggle.active)advanceWiggle();
}
void help(){Serial.println(
 "N20 degree test v7 | 8344 quadrature ticks / output revolution (PROVISIONAL)\n"
 "f degrees [power%] : forward by degrees; default power 100\n"
 "r degrees [power%] : reverse by degrees\n"
 "v turns [power%]   : signed revolutions; v 10 = ten forward, v -1 = reverse\n"
 "w [amplitude-deg] [cycles] : centered wiggle; defaults 2 degrees per side, 4 LR cycles\n"
 "s : stop immediately (no Enter needed); p : status; z : zero; h : help\n"
 "Examples: f 24 | r 5 | v 10 | v -10\n"
 "Limits: 20 turns per command; 500 ms encoder-stall timeout; 120 s total timeout.\n"
 "Runs at requested power, brakes early, then uses drive/brake pulses.\n"
 "Final brake 100 ms, release, report after 100 ms.\n"
 "No automatic reverse correction. Reversal includes gearbox/coupling backlash.\n"
 "Targets accumulate from the planned position; rounding and stop errors do not accumulate.\n"
 "Stop/fault rebases the plan to actual position. Zero does not move the tray.");}
// Shared nonblocking encoder motion primitive. loop() services the movement.
bool startEncoderMove(int32_t target,int power){
 if(phase!=IDLE)return false;
 startTicks=count();int32_t delta=target-startTicks;targetTicks=target;
 if(delta==0)return false;
 direction=delta>0?1:-1;completedTarget=false;
 coast();digitalWrite(ENABLE,HIGH);delay(2);requestedDuty=lround(power*255.0/100);pulseMs=3;pulseDone=false;
 if(abs(delta)<=APPROACH_TICKS){ledcWrite(0,255);ledcWrite(1,255);}else ledcWrite(direction>0?0:1,requestedDuty);
 phase=abs(delta)<=APPROACH_TICKS?CREEP_WAIT:MOVING;began=lastEdge=lastReport=phaseSince=millis();lastTicks=startTicks;
 return true;
}

// Public API: alternate left/right about the current position, then return to
// that position. amplitudeDegrees is the offset EACH side (2 => 4 deg L-to-R).
// Does not block. Returns false if busy or arguments are outside bench limits.
// The feeding plan is restored only after successful completion; stop/fault
// cancels the sequence and rebases to actual, just like a normal aborted move.
bool startWiggle(double amplitudeDegrees=2.0,uint8_t cycles=4){
 if(phase!=IDLE||wiggle.active||!isfinite(amplitudeDegrees)||amplitudeDegrees<0.1||amplitudeDegrees>5||cycles<1||cycles>10)return false;
 int32_t center=count(),amplitude=lround(amplitudeDegrees*TICKS_PER_REV/360);
 if(abs((int64_t)center)+amplitude>2000000000)return false;
 wiggle.active=true;wiggle.center=center;wiggle.savedPlan=plannedTicks;wiggle.amplitude=amplitude;wiggle.cycles=cycles;wiggle.leg=0;
 Serial.printf("WIGGLE start center=%ld amplitude=%.3f deg cycles=%u power=100%%\n",(long)center,amplitudeDegrees,cycles);
 startEncoderMove(center-amplitude,100);
 return true;
}
void advanceWiggle(){
 // Eight alternating extremes for four cycles, followed by the original center.
 while(++wiggle.leg<=2*wiggle.cycles){
  int32_t offset=wiggle.leg==2*wiggle.cycles?0:((wiggle.leg&1)?wiggle.amplitude:-wiggle.amplitude);
  if(startEncoderMove(wiggle.center+offset,100))return;
 }
 wiggle.active=false;plannedTicks=wiggle.savedPlan;
 Serial.printf("WIGGLE DONE center=%ld actual=%ld error=%ld ticks; feeding plan preserved.\n",(long)wiggle.center,(long)count(),(long)(count()-wiggle.center));
}

void command(){
 line[used]=0;char cmd=tolower(line[0]);
 if(cmd=='f'||cmd=='r'||cmd=='v'){
  if(phase!=IDLE){Serial.println("Wait for FINAL or press s.");return;}
  double amount=0,power=100;char extra=0;int n=sscanf(line+1,"%lf %lf %c",&amount,&power,&extra);
  if(n<1||n>2||!isfinite(amount)||!isfinite(power)||power<1||power>100||((cmd=='f'||cmd=='r')&&amount<=0)) {Serial.println("Use f/r positive-degrees [1..100 power] or v signed-turns [power].");return;}
  double degrees=cmd=='v'?amount*360:amount*(cmd=='r'?-1:1);
  if(fabs(degrees)>7200||fabs(degrees)<360.0/TICKS_PER_REV){Serial.println("Move must be at least one tick and at most 20 turns.");return;}
  double nextPlan=plannedTicks+degrees*TICKS_PER_REV/360;
  if(fabs(nextPlan)>2000000000){Serial.println("Position range exceeded; zero counters first.");return;}
  startTicks=count();int32_t nextTarget=lround(nextPlan);int32_t delta=nextTarget-startTicks;
  if((degrees>0&&delta<=0)||(degrees<0&&delta>=0)){Serial.println("Actual position already passed this planned target; use a larger move or z to rebase.");return;}
  plannedTicks=nextPlan;startEncoderMove(nextTarget,lround(power));
  Serial.printf("MOVE %.4f degrees at %.0f%% | start=%ld target=%ld delta=%ld\n",degrees,power,(long)startTicks,(long)targetTicks,(long)delta);
 }else if(cmd=='w'){
  double amplitude=2;unsigned cycles=4;char extra=0;
  int n=sscanf(line+1,"%lf %u %c",&amplitude,&cycles,&extra);
  if(n>2||cycles<1||cycles>10||!startWiggle(amplitude,(uint8_t)cycles))Serial.println("Wiggle requires idle motor, amplitude 0.1..5 degrees, cycles 1..10.");
 }else if(cmd=='p')report();
 else if(cmd=='z'){
  if(phase!=IDLE){Serial.println("Wait for FINAL before zeroing.");return;}
  portENTER_CRITICAL(&encLock);ticks=0;edgesA=edgesB=invalid=0;portEXIT_CRITICAL(&encLock);plannedTicks=0;report();
 }else if(cmd=='h'||cmd=='?')help();else if(cmd)Serial.println("Unknown command; h for help.");
}
void setup(){
 pinMode(ENABLE,OUTPUT);digitalWrite(ENABLE,LOW);pinMode(IN1,OUTPUT);pinMode(IN2,OUTPUT);digitalWrite(IN1,LOW);digitalWrite(IN2,LOW);
 ledcSetup(0,20000,8);ledcSetup(1,20000,8);ledcAttachPin(IN1,0);ledcAttachPin(IN2,1);coast();
 pinMode(ENC_A,INPUT_PULLUP);pinMode(ENC_B,INPUT_PULLUP);previous=(digitalRead(ENC_A)<<1)|digitalRead(ENC_B);
 attachInterrupt(digitalPinToInterrupt(ENC_A),encoder,CHANGE);attachInterrupt(digitalPinToInterrupt(ENC_B),encoder,CHANGE);
 Serial.begin(115200);delay(500);help();
}
void loop(){
 uint32_t now=millis();int32_t t=count();
 if(phase!=IDLE&&!Serial){
  if(!disconnectedSince)disconnectedSince=now;
  if(now-disconnectedSince>=100){coast();phase=IDLE;plannedTicks=count();wiggle.active=false;disconnectStopPending=true;}
 }else disconnectedSince=0;
 if(Serial&&disconnectStopPending){Serial.println("ABORT: USB console disconnected; motor disabled before target.");disconnectStopPending=false;report();}
 bool fastWiggle=wiggle.active&&wiggle.leg<2*wiggle.cycles;
 bool approaching=phase==MOVING||phase==CREEP_WAIT||phase==CREEP_DRIVE;
 if(approaching){
  if(t!=lastTicks){lastTicks=t;lastEdge=now;}
  int64_t remaining=(int64_t)(targetTicks-t)*direction;
  if((int64_t)(t-startTicks)*direction < -32)brake("FAULT: encoder direction opposite commanded direction.");
  else if(remaining<=0)brake("Target reached; braking.",true);
  else if(now-began>=120000)brake("FAULT: total move timeout.");
  else if(now-lastEdge>=500)brake("FAULT: no encoder motion for 500 ms.");
  else if(phase==MOVING){
   if(remaining<=APPROACH_TICKS){ledcWrite(0,255);ledcWrite(1,255);phase=CREEP_WAIT;phaseSince=now;pulseDone=false;}
   else ledcWrite(direction>0?0:1,requestedDuty);
  }else if(phase==CREEP_WAIT&&now-phaseSince>=(fastWiggle?(pulseDone?8:10):(pulseDone?PULSE_BRAKE_MS:INITIAL_BRAKE_MS))){
   // If the last pulse could not overcome static friction, lengthen it slightly.
   if(pulseDone){if(t==pulseStart)pulseMs=min(pulseMs+1,(uint32_t)12);else if(abs(t-pulseStart)>6)pulseMs=max((uint32_t)2,pulseMs-1);}
   drivePulseMs=fastWiggle&&remaining>20?max(pulseMs,(uint32_t)5):pulseMs;
   pulseStart=t;pulseDone=false;
   ledcWrite(0,0);ledcWrite(1,0);ledcWrite(direction>0?0:1,requestedDuty);
   phase=CREEP_DRIVE;phaseSince=now;
  }else if(phase==CREEP_DRIVE&&now-phaseSince>=drivePulseMs){
   ledcWrite(0,255);ledcWrite(1,255);phase=CREEP_WAIT;phaseSince=now;pulseDone=true;
  }
 }else if(phase==BRAKING){
  bool intermediate=wiggle.active&&completedTarget&&wiggle.leg<2*wiggle.cycles;
  if(intermediate&&now-phaseSince>=20){finish();}
  else if(!intermediate&&now-phaseSince>=FINAL_BRAKE_MS){coast();phase=SETTLING;phaseSince=now;}
 }
 else if(phase==SETTLING&&now-phaseSince>=FINAL_SETTLE_MS)finish();
 for(int i=0;i<32&&Serial.available();i++){
  char c=Serial.read();
  if(c=='s'||c=='S'||c==27){used=0;discard=false;if(phase!=IDLE)brake("USER STOP; braking.");else {coast();plannedTicks=count();Serial.println("Stopped; plan rebased.");}continue;}
  if(c=='\n'||c=='\r'){if(used&&!discard)command();used=0;discard=false;}
  else if(!discard&&used<sizeof(line)-1)line[used++]=c;
  else if(!discard){discard=true;used=0;if(phase!=IDLE)brake("FAULT: input too long.");}
 }
 if(approaching&&now-lastReport>=1000){lastReport=now;report();}
 delay(1);
}
