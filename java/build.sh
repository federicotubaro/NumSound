javac -d build/classes src/NumSound.java
echo "Main-Class: NumSound" > build/manifest.txt
jar cfm build/NumSound.jar build/manifest.txt -C build/classes .
