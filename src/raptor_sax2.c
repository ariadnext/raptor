/* -*- Mode: c; c-basic-offset: 2 -*-
 *
 * raptor_sax2.c - Raptor SAX2 API
 *
 * $Id$
 *
 * Copyright (C) 2000-2004, David Beckett http://purl.org/net/dajobe/
 * Institute for Learning and Research Technology http://www.ilrt.bristol.ac.uk/
 * University of Bristol, UK http://www.bristol.ac.uk/
 * 
 * This package is Free Software and part of Redland http://librdf.org/
 * 
 * It is licensed under the following three licenses as alternatives:
 *   1. GNU Lesser General Public License (LGPL) V2.1 or any newer version
 *   2. GNU General Public License (GPL) V2 or any newer version
 *   3. Apache License, V2.0 or any newer version
 * 
 * You may not use this file except in compliance with at least one of
 * the above three licenses.
 * 
 * See LICENSE.html or LICENSE.txt at the top of this package for the
 * complete terms and further detail along with the license texts for
 * the licenses in COPYING.LIB, COPYING and LICENSE-2.0.txt respectively.
 * 
 * 
 */


#ifdef HAVE_CONFIG_H
#include <raptor_config.h>
#endif

#ifdef WIN32
#include <win32_raptor_config.h>
#endif


#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

/* Raptor includes */
#include "raptor.h"
#include "raptor_internal.h"


/* Define this for far too much output */
#undef RAPTOR_DEBUG_CDATA




raptor_sax2_element*
raptor_sax2_element_pop(raptor_sax2 *sax2) 
{
  raptor_sax2_element *element=sax2->current_element;

  if(!element)
    return NULL;

  sax2->current_element=element->parent;
  if(sax2->root_element == element) /* just deleted root */
    sax2->root_element=NULL;

  return element;
}


void
raptor_sax2_element_push(raptor_sax2 *sax2, raptor_sax2_element* element) 
{
  element->parent=sax2->current_element;
  sax2->current_element=element;
  if(!sax2->root_element)
    sax2->root_element=element;
}


raptor_sax2_element*
raptor_new_sax2_element(raptor_qname *name,
                        const unsigned char *xml_language, 
                        raptor_uri *xml_base) {
  raptor_sax2_element* sax2_element;

  sax2_element=(raptor_sax2_element*)RAPTOR_CALLOC(raptor_sax2_element, 1,
                                                   sizeof(raptor_sax2_element));
  if(!sax2_element)
    return NULL;

  /* Element name */
  sax2_element->name=name;
  sax2_element->xml_language=xml_language;
  sax2_element->base_uri=xml_base;

  return sax2_element;
}


void
raptor_free_sax2_element(raptor_sax2_element *element)
{
  unsigned int i;

  for (i=0; i < element->attribute_count; i++)
    if(element->attributes[i])
      raptor_free_qname(element->attributes[i]);

  if(element->attributes)
    RAPTOR_FREE(raptor_qname_array, element->attributes);

  if(element->content_cdata_length)
    RAPTOR_FREE(raptor_qname_array, element->content_cdata);

  if(element->base_uri)
    raptor_free_uri(element->base_uri);

  if(element->xml_language)
    RAPTOR_FREE(cstring, element->xml_language);

  raptor_free_qname(element->name);
  RAPTOR_FREE(raptor_element, element);
}


#ifdef RAPTOR_DEBUG
void
raptor_print_sax2_element(raptor_sax2_element *element, FILE* stream)
{
  raptor_qname_print(stream, element->name);
  fputc('\n', stream);

  if(element->attribute_count) {
    unsigned int i;

    fputs(" attributes: ", stream);
    for (i = 0; i < element->attribute_count; i++) {
      if(i)
        fputc(' ', stream);
      raptor_qname_print(stream, element->attributes[i]);
      fprintf(stream, "='%s'", element->attributes[i]->value);
    }
    fputc('\n', stream);
  }
}
#endif


struct nsd
{
  const raptor_namespace *nspace;
  unsigned char *declaration;
  size_t length;
};


static int
raptor_nsd_compare(const void *a, const void *b) 
{
  struct nsd* nsd_a=(struct nsd*)a;
  struct nsd* nsd_b=(struct nsd*)b;
  return strcmp((const char*)nsd_a->declaration, (const char*)nsd_b->declaration);
}


unsigned char *
raptor_format_sax2_element(raptor_sax2_element *element,
                           raptor_namespace_stack *nstack,
                           size_t *length_p, int is_end,
                           raptor_simple_message_handler error_handler,
                           void *error_data,
                           int depth)
{
  size_t length;
  unsigned char *buffer;
  unsigned char *ptr;
  struct nsd *nspace_declarations;
  size_t nspace_declarations_count=0;  
  unsigned int i;

  /* max is 1 per element and 1 for each attribute */
  if(nstack)
    nspace_declarations=(struct nsd*)RAPTOR_CALLOC(nsdarray, element->attribute_count+1, sizeof(struct nsd));

  /* get length of element name (and namespace-prefix: if there is one) */
  length=element->name->local_name_length + 1; /* < */
  if(element->name->nspace) {
    if(element->name->nspace->prefix_length > 0)
      length += element->name->nspace->prefix_length + 1; /* : */

    if(!is_end && nstack &&
       !raptor_namespaces_namespace_in_scope(nstack, element->name->nspace)) {
      nspace_declarations[0].declaration=
        raptor_namespaces_format(element->name->nspace,
                                 &nspace_declarations[0].length);
      nspace_declarations[0].nspace=element->name->nspace;
      nspace_declarations_count++;
      length += nspace_declarations[0].length+1; /* plus space */
    }
  }

  if(is_end)
    length++; /* / */

  if (!is_end && element->attributes) {
    for(i=0; i < element->attribute_count; i++) {
      size_t escaped_attr_val_len;
      
      length++; /* ' ' between attributes and after element name */

      /* qname */
      length += element->attributes[i]->local_name_length;
      if(element->attributes[i]->nspace) {
        if(element->attributes[i]->nspace->prefix_length > 0)
          length += element->attributes[i]->nspace->prefix_length + 1; /* prefix: */

        if(nstack && 
           !raptor_namespaces_namespace_in_scope(nstack, element->attributes[i]->nspace) && element->attributes[i]->nspace != element->name->nspace) {
          /* not in scope and not same as element (so already going to be declared)*/
          unsigned int j;
          int declare_me=1;
          
          /* check it wasn't an earlier declaration too */
          for (j=0; j < nspace_declarations_count; j++)
            if(nspace_declarations[j].nspace == element->attributes[j]->nspace) {
              declare_me=0;
              break;
            }
            
          if(declare_me) {
            nspace_declarations[nspace_declarations_count].declaration=
              raptor_namespaces_format(element->attributes[i]->nspace,
                                       &nspace_declarations[nspace_declarations_count].length);
            nspace_declarations[nspace_declarations_count].nspace=element->attributes[i]->nspace;
            length += nspace_declarations[nspace_declarations_count].length+1; /* plus space */
            nspace_declarations_count++;
          }
        }

      }
      

      /* XML escaped value */
      escaped_attr_val_len=raptor_xml_escape_string(element->attributes[i]->value, element->attributes[i]->value_length,
                                                    NULL, 0, '"',
                                                    error_handler, error_data);
      /* ="XML-escaped-value" */
      length += 3+escaped_attr_val_len;
    }
  }
  
  length++; /* > */
  
  if(length_p)
    *length_p=length;

  /* +1 here is for \0 at end */
  buffer=(unsigned char*)RAPTOR_MALLOC(cstring, length + 1);
  if(!buffer)
    return NULL;

  ptr=buffer;

  *ptr++ = '<';
  if(is_end)
    *ptr++ = '/';
  if(element->name->nspace && element->name->nspace->prefix_length > 0) {
    strncpy((char*)ptr, (const char*)element->name->nspace->prefix,
            element->name->nspace->prefix_length);
    ptr+= element->name->nspace->prefix_length;
    *ptr++=':';
  }
  strcpy((char*)ptr, (const char*)element->name->local_name);
  ptr += element->name->local_name_length;

  /* declare namespaces */
  if(nspace_declarations_count) {
    /* sort them into the canonical order */
    qsort((void*)nspace_declarations, 
          nspace_declarations_count, sizeof(struct nsd),
          raptor_nsd_compare);
    /* add them */
    for (i=0; i < nspace_declarations_count; i++) {
      *ptr++=' ';
      strncpy((char*)ptr, (const char*)nspace_declarations[i].declaration,
              nspace_declarations[i].length);
      RAPTOR_FREE(cstring, nspace_declarations[i].declaration);
      nspace_declarations[i].declaration=NULL;
      ptr+=nspace_declarations[i].length;

      raptor_namespace_copy(nstack,
                            (raptor_namespace*)nspace_declarations[i].nspace,
                            depth);
    }
  }


  if(!is_end && element->attributes) {
    for(i=0; i < element->attribute_count; i++) {
      size_t escaped_attr_val_len;

      *ptr++ =' ';
      
      if(element->attributes[i]->nspace && 
         element->attributes[i]->nspace->prefix_length > 0) {
        strncpy((char*)ptr, (char*)element->attributes[i]->nspace->prefix,
                element->attributes[i]->nspace->prefix_length);
        ptr+= element->attributes[i]->nspace->prefix_length;
        *ptr++=':';
      }
    
      strcpy((char*)ptr, (const char*)element->attributes[i]->local_name);
      ptr += element->attributes[i]->local_name_length;
      
      *ptr++ ='=';
      *ptr++ ='"';
      
      escaped_attr_val_len=raptor_xml_escape_string(element->attributes[i]->value, element->attributes[i]->value_length,
                                                    NULL, 0, '"',
                                                    error_handler, error_data);
      if(escaped_attr_val_len == element->attributes[i]->value_length) {
        /* save a malloc/free when there is no escaping */
        strcpy((char*)ptr, (const char*)element->attributes[i]->value);
        ptr += element->attributes[i]->value_length;
      } else {
        unsigned char *escaped_attr_val=(unsigned char*)RAPTOR_MALLOC(cstring,escaped_attr_val_len+1);
        raptor_xml_escape_string(element->attributes[i]->value, element->attributes[i]->value_length,
                                 escaped_attr_val, escaped_attr_val_len, '"',
                                 error_handler, error_data);
        
        strcpy((char*)ptr, (const char*)escaped_attr_val);
        RAPTOR_FREE(cstring,escaped_attr_val);
        ptr += escaped_attr_val_len;
      }
      *ptr++ ='"';
    }
  }
  
  *ptr++ = '>';
  *ptr='\0';

  if(nstack)
    RAPTOR_FREE(stringarray, nspace_declarations);

  return buffer;
}
