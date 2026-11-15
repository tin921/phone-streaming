# Decodes the QR images qr_test.exe wrote and compares them with the texts.
#   py qr_check.py < output of qr_test.exe
import sys
import cv2

failures = 0
detector = cv2.QRCodeDetector()
for line in sys.stdin:
    line = line.rstrip('\r\n')
    if not line:
        continue
    file, version, text = line.split('\t', 2)
    image = cv2.imread(file)
    got, _, _ = detector.detectAndDecode(image)
    ok = got == text
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  version {version:>2}  {len(text):3} chars" + ('' if ok else f"  got {got!r}"))
print(f"{'all decoded' if not failures else str(failures) + ' failed'}")
sys.exit(1 if failures else 0)
