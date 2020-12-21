# DjangoImports

from django.urls import path

# End DjangoImports


# InternalImports

from . import views

# End InternalImports

urlpatterns = [
    path('', views.index, name='index'),
    path('send_sms/', views.sending_sms, name='send_sms'),
    path('success/', views.success, name='success'),

    # ContactCRUD

    path('contacts/add/', views.contact_add, name='contact_add'),
    path('contacts/', views.contact_list, name='contact_list'),
    path('contacts/<int:contact_id>/edit/', views.contact_edit, name='contact_edit'),
    path('contacts/<int:contact_id>/delete/', views.contact_delete, name='contact_delete'),

    # End ContactCRUD

]
