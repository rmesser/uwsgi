#include "psgi.h"

extern struct uwsgi_server uwsgi;

extern struct http_status_codes hsc[];

int psgi_response(struct wsgi_request *wsgi_req, AV *response) {

	SV **status_code, **hitem ;
	AV *headers, *body =NULL;
	STRLEN hlen;
	struct http_status_codes *http_sc;
	int i,vi, base;
	char *chitem;
	SV **harakiri;

#ifdef UWSGI_ASYNC
	if (wsgi_req->async_status == UWSGI_AGAIN) {

		wsgi_req->async_force_again = 0;

		wsgi_req->switches++;
                SV *chunk = uwsgi_perl_obj_call(wsgi_req->async_placeholder, "getline");
		if (!chunk) {
			internal_server_error(wsgi_req, "exception raised");
			return UWSGI_OK;
		}

		if (wsgi_req->async_force_again) {
			SvREFCNT_dec(chunk);
			return UWSGI_AGAIN;
		}

                chitem = SvPV( chunk, hlen);

                if (hlen <= 0) {
			SvREFCNT_dec(chunk);
			SV *closed = uwsgi_perl_obj_call(wsgi_req->async_placeholder, "close");
                	if (closed) {
                        	SvREFCNT_dec(closed);
                	}

			// check for psgix.harakiri
        		harakiri = hv_fetch((HV*)SvRV( (SV*)wsgi_req->async_environ), "psgix.harakiri.commit", 21, 0);
        		if (harakiri) {
                		if (SvTRUE(*harakiri)) wsgi_req->async_plagued = 1;
        		}

			SvREFCNT_dec(wsgi_req->async_environ);
        		SvREFCNT_dec(wsgi_req->async_result);

			return UWSGI_OK;
                }

                wsgi_req->response_size += wsgi_req->socket->proto_write(wsgi_req, chitem, hlen);
		SvREFCNT_dec(chunk);

		return UWSGI_AGAIN;
	}
#endif

	if (SvTYPE(response) != SVt_PVAV) {
		uwsgi_log("invalid PSGI response type\n");
		return UWSGI_OK;
	}

	status_code = av_fetch(response, 0, 0);
	if (!status_code) { uwsgi_log("invalid PSGI status code\n"); return UWSGI_OK;}

	char *status_str = SvPV(*status_code, hlen);
	if (uwsgi_response_prepare_headers(wsgi_req, status_str, hlen)) return UWSGI_OK;

        hitem = av_fetch(response, 1, 0);
	if (!hitem) { uwsgi_log("invalid PSGI headers\n"); return UWSGI_OK;}

        headers = (AV *) SvRV(*hitem);
	if (!headers) { uwsgi_log("invalid PSGI headers\n"); return UWSGI_OK;}

        base = 5;


        // put them in hvec
        for(i=0; i<=av_len(headers); i++) {
                vi = (i*2)+base;
                hitem = av_fetch(headers,i,0);
                chitem = SvPV(*hitem, hlen);
                hitem2 = av_fetch(headers,i+1,0);
                chitem2 = SvPV(*hitem, hlen);
		if (uwsgi_response_add_header(wsgi_req, chitem, hlen, chitem2, hlen)) return UWSGI_OK;
        }

        hitem = av_fetch(response, 2, 0);

	if (!hitem) {
		return UWSGI_OK;
	}

	if (!SvRV(*hitem)) { uwsgi_log("invalid PSGI response body\n") ; return UWSGI_OK; }
	
        if (SvTYPE(SvRV(*hitem)) == SVt_PVGV || SvTYPE(SvRV(*hitem)) == SVt_PVHV || SvTYPE(SvRV(*hitem)) == SVt_PVMG) {

		// respond to fileno ?
		if (uwsgi.async < 2) {
			// check for fileno() method, IO class or GvIO
			if (uwsgi_perl_obj_can(*hitem, "fileno", 6) || uwsgi_perl_obj_isa(*hitem, "IO") || (uwsgi_perl_obj_isa(*hitem, "GLOB") && GvIO(SvRV(*hitem)))  ) {
				SV *fn = uwsgi_perl_obj_call(*hitem, "fileno");
				if (fn) {
					if (SvTYPE(fn) == SVt_IV && SvIV(fn) >= 0) {
						wsgi_req->sendfile_fd = SvIV(fn);
						SvREFCNT_dec(fn);	
						wsgi_req->response_size += uwsgi_sendfile(wsgi_req);
						// no need to close here as perl GC will do the close()
						return UWSGI_OK;
					}
					SvREFCNT_dec(fn);	
				}
			}
			
			// check for path method
			if (uwsgi_perl_obj_can(*hitem, "path", 4)) {
				SV *p = uwsgi_perl_obj_call(*hitem, "path");
				wsgi_req->sendfile_fd = open(SvPV_nolen(p), O_RDONLY);
				SvREFCNT_dec(p);	
				wsgi_req->response_size += uwsgi_sendfile(wsgi_req);
				close(wsgi_req->sendfile_fd);
				return UWSGI_OK;
			}
		}

                for(;;) {

			wsgi_req->switches++;
                        SV *chunk = uwsgi_perl_obj_call(*hitem, "getline");
			if (!chunk) {
				internal_server_error(wsgi_req, "exception raised");
				break;
			}

                        chitem = SvPV( chunk, hlen);
#ifdef UWSGI_ASYNC
			if (uwsgi.async > 1 && wsgi_req->async_force_again) {
				SvREFCNT_dec(chunk);
				wsgi_req->async_status = UWSGI_AGAIN;
				wsgi_req->async_placeholder = (SV *) *hitem;
				return UWSGI_AGAIN;
			}
#endif
                        if (hlen <= 0) {
				SvREFCNT_dec(chunk);
                                break;
                        }
                        wsgi_req->response_size += wsgi_req->socket->proto_write(wsgi_req, chitem, hlen);
			SvREFCNT_dec(chunk);
#ifdef UWSGI_ASYNC
			if (uwsgi.async > 1) {
				wsgi_req->async_status = UWSGI_AGAIN;
				wsgi_req->async_placeholder = (SV *) *hitem;
				return UWSGI_AGAIN;
			}
#endif
                }


		SV *closed = uwsgi_perl_obj_call(*hitem, "close");
		if (closed) {
			SvREFCNT_dec(closed);
		}

        }
        else if (SvTYPE(SvRV(*hitem)) == SVt_PVAV)  {

                body = (AV *) SvRV(*hitem);

                for(i=0; i<=av_len(body); i++) {
                        hitem = av_fetch(body,i,0);
                        chitem = SvPV(*hitem, hlen);
                        wsgi_req->response_size += wsgi_req->socket->proto_write(wsgi_req, chitem, hlen);
                }

        }
        else {
                uwsgi_log("unsupported response body type: %d\n", SvTYPE(SvRV(*hitem)));
        }
	
	return UWSGI_OK;

}
