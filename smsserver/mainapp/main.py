import os
from django.conf.urls.static import static
from subprocess import call, Popen, CREATE_NEW_CONSOLE


def send_sms():
    p = Popen('C:\\Users\\E-MaxUser\\Desktop\\files\\ne\\smsserver\\static\\sms\\a_1.bat',
              cwd='C:\\Users\\E-MaxUser\\Desktop\\files\\ne\\smsserver\\static\\sms',
              creationflags=CREATE_NEW_CONSOLE)
    return p
