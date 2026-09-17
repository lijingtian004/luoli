#!/bin/bash
# Luoli build environment setup (Ubuntu 22.04)
exec > /root/setup.log 2>&1
set -x
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq openjdk-17-jdk-headless unzip wget > /dev/null
java -version

# Android cmdline-tools
mkdir -p /opt/android-sdk/cmdline-tools
cd /tmp
wget -q https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip -O cmdtools.zip
unzip -q -o cmdtools.zip -d /opt/android-sdk/cmdline-tools
mv /opt/android-sdk/cmdline-tools/cmdline-tools /opt/android-sdk/cmdline-tools/latest

export ANDROID_HOME=/opt/android-sdk
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$PATH
yes | sdkmanager --licenses > /dev/null
sdkmanager "platform-tools" "platforms;android-34" "build-tools;34.0.0" "ndk;26.3.11579264" "cmake;3.22.1" > /dev/null

# Gradle
wget -q https://services.gradle.org/distributions/gradle-8.7-bin.zip -O gradle.zip
unzip -q -o gradle.zip -d /opt
ln -sf /opt/gradle-8.7/bin/gradle /usr/local/bin/gradle
gradle --version

echo DONE > /root/setup.done
