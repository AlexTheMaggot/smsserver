from django.urls import path
from . import views

urlpatterns = [
    path('', views.index, name='index'),
    path('send_sms', views.sending_sms, name='send_sms'),
    path('success', views.success, name='success'),
]
