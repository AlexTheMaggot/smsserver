# DjangoImports

from django.db import models

# End DjangoImports


# ContactsModel

class Contact(models.Model):
    phone = models.IntegerField(max_length=12)

# End ContactsModel