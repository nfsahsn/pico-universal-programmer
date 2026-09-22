// Minimal compile/upload smoke test. LED polarity/pin depends on the selected board.
void setup() {
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif
}

void loop() {
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, HIGH);
#endif
  delay(500);
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, LOW);
#endif
  delay(500);
}
