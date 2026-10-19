@echo off
rem Kokoro says every name in RoundTripNames.txt, Whisper listens: RoundTrip_report.tsv + HeardAs.suggested.txt (no game needed)
cd /d "%~dp0"
VoiceCommand.exe --roundtrip
