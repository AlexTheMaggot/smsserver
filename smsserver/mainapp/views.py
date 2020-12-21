# DjangoImports

from django.shortcuts import render, redirect

# End DjangoImports


# InternalImports

from .main import send_sms

# End InternalImports


# IndexView

def index(request):
    template = 'mainapp/index.html'
    return render(request, template)

# End IndexView


# SendSMSView

def sending_sms(request):
    url = 'success'
    send_sms()
    return redirect(url)

# End SendSMSView


# SuccessView

def success(request):
    template = 'mainapp/success.html'
    return render(request, template)

# End SuccessView
