"""
Quick WAV recorder for testing Whisper
Requires: pip install sounddevice scipy
"""
import sounddevice as sd
import scipy.io.wavfile as wav
import numpy as np

print("Recording 10 seconds of audio...")
print("Speak now: 'This is a test of Whisper speech recognition'")
print()

# Record 10 seconds at 48kHz
duration = 10
sample_rate = 48000

recording = sd.rec(int(duration * sample_rate), 
                   samplerate=sample_rate, 
                   channels=2, 
                   dtype='float32')
sd.wait()

print("Recording complete! Saving...")

# Save as WAV
output_file = "speech.wav"  # Saved in the current folder
wav.write(output_file, sample_rate, recording)

print(f"Saved to: {output_file}")
print("Run: .\\WhisperTest.exe speech.wav")
