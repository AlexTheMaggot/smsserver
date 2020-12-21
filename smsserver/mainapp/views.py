# DjangoImports

from django.shortcuts import render, redirect, get_object_or_404

# End DjangoImports


# InternalImports

from .main import send_sms
from .models import Contact

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


# ContactCRUDViews

def contact_add(request):
    if request.method == 'GET':
        template = 'mainapp/contact/contact_add.html'
        return render(request, template)
    elif request.method == 'POST':
        contact = Contact(phone=request.POST['phone'])
        contact.save()
        url = 'contact_list'
        return redirect(url)


def contact_list(request):
    contacts = Contact.objects.all().filter('id')
    context = {
        'contacts': contacts,
    }
    template = 'mainapp/contact/contact_list.html'
    return render(request, template, context)


def contact_edit(request, contact_id):
    contact = get_object_or_404(Contact, id=contact_id)
    if request.method == 'GET':
        context = {
            'contact': contact,
        }
        template = 'mainapp/contact/contact_edit.html'
        return render(request, template, context)
    elif request.method == 'POST':
        contact.phone = request.POST['phone']
        contact.save()
        url = 'contact_list'
        return redirect(url)


def contact_delete(request, contact_id):
    contact = get_object_or_404(Contact, id=contact_id)
    contact.delete()
    url = 'contact_list'
    return redirect(url)

# End ContactCRUDViews
