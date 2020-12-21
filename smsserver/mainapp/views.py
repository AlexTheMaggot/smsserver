from django.shortcuts import render, redirect
from .main import send_sms


def index(request):
    template = 'mainapp/index.html'
    return render(request, template)


def sending_sms(request):
    url = 'success'
    send_sms()
    return redirect(url)


def success(request):
    template = 'mainapp/success.html'
    return render(request, template)