#include <krb5_locl.h>

RCSID("$Id$");

krb5_error_code
krb5_build_authenticator (krb5_context context,
			  krb5_auth_context auth_context,
			  krb5_creds *cred,
			  Checksum *cksum,
			  Authenticator **auth_result,
			  krb5_data *result)
{
  struct timeval tv;
  Authenticator *auth = malloc(sizeof(*auth));
  char buf[1024];
  size_t len;
  krb5_error_code ret;
  unsigned seq_number;

  auth->authenticator_vno = 5;
#ifdef USE_ASN1_PRINCIPAL
  copy_Realm(&cred->client->realm, &auth->crealm);
  copy_PrincipalName(&cred->client->name, &auth->cname);
#else
  auth->crealm = malloc(cred->client->realm.length + 1);
  strncpy(auth->crealm, cred->client->realm.data, cred->client->realm.length);
  auth->crealm[cred->client->realm.length] = '\0';
  krb5_principal2principalname(&auth->cname, cred->client);
#endif

  gettimeofday(&tv, NULL);
  auth->cusec = tv.tv_usec;
  auth->ctime = tv.tv_sec;
#if 0
  auth->subkey = NULL;
#else
  krb5_generate_subkey (context, &cred->session, &auth->subkey);
  copy_EncryptionKey (auth->subkey,
		      &auth_context->local_subkey);
#endif
  if (auth_context->flags & KRB5_AUTH_CONTEXT_DO_SEQUENCE) {
    krb5_generate_seq_number (context,
			      &cred->session, 
			      &auth_context->local_seqnumber);
    seq_number = auth_context->local_seqnumber;
    auth->seq_number = &seq_number;
  } else
    auth->seq_number = NULL;
  auth->authorization_data = NULL;
  auth->cksum = cksum;

  /* XXX - Copy more to auth_context? */

  if (auth_context) {
    auth_context->authenticator->cusec = tv.tv_usec;
    auth_context->authenticator->ctime = tv.tv_sec;
  }

  memset (buf, 0, sizeof(buf));
  ret = encode_Authenticator (buf + sizeof(buf) - 1, sizeof(buf), auth, &len);

  ret = krb5_encrypt (context, buf + sizeof(buf) - len, len,
		      auth_context->enctype,
		      &cred->session,
		      result);

  if (auth_result)
    *auth_result = auth;
  else {
    free (auth->crealm);
    free (auth);
  }
  return ret;
}
