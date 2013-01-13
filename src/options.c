#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <confuse/confuse.h>

static void cfg_indent(FILE *fp, int indent)
{
    while(indent--)
        fprintf(fp, "  ");
}

void cfg_init_defaults(cfg_t *cfg)
{
    int i;

    for(i = 0; cfg->opts[i].name; i++)
    {
        /* libConfuse doesn't handle default values for "simple" options */
        if(cfg->opts[i].simple_value.ptr || is_set(CFGF_NODEFAULT, cfg->opts[i].flags))
            continue;

        if(cfg->opts[i].type != CFGT_SEC)
        {
            cfg->opts[i].flags |= CFGF_DEFINIT;

            if(is_set(CFGF_LIST, cfg->opts[i].flags) ||
               cfg->opts[i].def.parsed)
            {
                int xstate, ret;

                /* If it's a list, but no default value was given,
                 * keep the option uninitialized.
                 */
                if(cfg->opts[i].def.parsed == 0 ||
                   cfg->opts[i].def.parsed[0] == 0)
                    continue;

                /* setup scanning from the string specified for the
                 * "default" value, force the correct state and option
                 */

                if(is_set(CFGF_LIST, cfg->opts[i].flags))
                    /* lists must be surrounded by {braces} */
                    xstate = 3;
                else if(cfg->opts[i].type == CFGT_FUNC)
                    xstate = 0;
                else
                    xstate = 2;

                cfg_scan_string_begin(cfg->opts[i].def.parsed);
                do
                {
                    ret = cfg_parse_internal(cfg, 1, xstate, &cfg->opts[i]);
                    xstate = -1;
                } while(ret == STATE_CONTINUE);
                cfg_scan_string_end();
                if(ret == STATE_ERROR)
                {
                    /*
                     * If there was an error parsing the default string,
                     * the initialization of the default value could be
                     * inconsistent or empty. What to do? It's a
                     * programming error and not an end user input
                     * error. Lets print a message and abort...
                     */
                    fprintf(stderr, "Parse error in default value '%s'"
                            " for option '%s'\n",
                            cfg->opts[i].def.parsed, cfg->opts[i].name);
                    fprintf(stderr, "Check your initialization macros and the"
                            " libConfuse documentation\n");
                    abort();
                }
            }
            else
            {
                switch(cfg->opts[i].type)
                {
                    case CFGT_INT:
                        cfg_opt_setnint(&cfg->opts[i],
                                        cfg->opts[i].def.number, 0);
                        break;
                    case CFGT_FLOAT:
                        cfg_opt_setnfloat(&cfg->opts[i],
                                          cfg->opts[i].def.fpnumber, 0);
                        break;
                    case CFGT_BOOL:
                        cfg_opt_setnbool(&cfg->opts[i],
                                         cfg->opts[i].def.boolean, 0);
                        break;
                    case CFGT_STR:
                        cfg_opt_setnstr(&cfg->opts[i],
                                        cfg->opts[i].def.string, 0);
                        break;
                    case CFGT_FUNC:
                    case CFGT_PTR:
                        break;
                    default:
                        cfg_error(cfg,
                                  "internal error in cfg_init_defaults(%s)",
                                  cfg->opts[i].name);
                        break;
                }
            }

            /* The default value should only be returned if no value
             * is given in the configuration file, so we set the RESET
             * flag here. When/If cfg_setopt() is called, the value(s)
             * will be freed and the flag unset.
             */
            cfg->opts[i].flags |= CFGF_RESET;
        } /* end if cfg->opts[i].type != CFGT_SEC */
        else if(!is_set(CFGF_MULTI, cfg->opts[i].flags))
        {
            cfg_setopt(cfg, &cfg->opts[i], 0);
            cfg->opts[i].flags |= CFGF_DEFINIT;
        }
    }
}

cfg_value_t *_cfg_addval(cfg_opt_t *opt)
{
    opt->values = realloc(opt->values,
                                  (opt->nvalues+1) * sizeof(cfg_value_t *));
    assert(opt->values);
    opt->values[opt->nvalues] = malloc(sizeof(cfg_value_t));
    memset(opt->values[opt->nvalues], 0, sizeof(cfg_value_t));
    return opt->values[opt->nvalues++];
}

DLLIMPORT int cfg_numopts(cfg_opt_t *opts)
{
    int n;

    for(n = 0; opts[n].name; n++) 
      {
	printf("opts name=%s\n", opts[n].name);
      }
        /* /\* do nothing *\/ ; */
    return n;
}

cfg_opt_t *cfg_dupopt_array(cfg_opt_t *opts)
{
    int i;
    cfg_opt_t *dupopts;

    int n = cfg_numopts(opts);

    dupopts = calloc(n+1, sizeof(cfg_opt_t));
    memcpy(dupopts, opts, n * sizeof(cfg_opt_t));

    for(i = 0; i < n; i++)
    {
        dupopts[i].name = strdup(opts[i].name);
        if(opts[i].type == CFGT_SEC && opts[i].subopts)
            dupopts[i].subopts = cfg_dupopt_array(opts[i].subopts);

        if(is_set(CFGF_LIST, opts[i].flags) || opts[i].type == CFGT_FUNC)
            dupopts[i].def.parsed = opts[i].def.parsed ? strdup(opts[i].def.parsed) : 0;
        else if(opts[i].type == CFGT_STR)
            dupopts[i].def.string = opts[i].def.string ? strdup(opts[i].def.string) : 0;
    }

    return dupopts;
}

DLLIMPORT cfg_value_t *
cfg_setopt(cfg_t *cfg, cfg_opt_t *opt, char *value)
{
    cfg_value_t *val = 0;
    int b;
    char *s;
    double f;
    long int i;
    void *p;
    char *endptr;

    assert(cfg && opt);

    if(opt->simple_value.ptr)
    {
        assert(opt->type != CFGT_SEC);
        val = (cfg_value_t *)opt->simple_value.ptr;
    }
    else
    {
        if(is_set(CFGF_RESET, opt->flags))
        {
            cfg_free_value(opt);
            opt->flags &= ~CFGF_RESET;
        }

        if(opt->nvalues == 0 || is_set(CFGF_MULTI, opt->flags) ||
           is_set(CFGF_LIST, opt->flags))
        {
            val = 0;
            if(opt->type == CFGT_SEC && is_set(CFGF_TITLE, opt->flags))
            {
                unsigned int i;

                /* Check if there already is a section with the same title.
                 */

		/* Assert that there are either no sections at all, or a
		 * non-NULL section title. */
                assert(opt->nvalues == 0 || value);

                for(i = 0; i < opt->nvalues && val == NULL; i++)
                {
                    cfg_t *sec = opt->values[i]->section;
                    if(is_set(CFGF_NOCASE, cfg->flags))
                    {
                        if(strcasecmp(value, sec->title) == 0)
                            val = opt->values[i];
                    }
                    else
                    {
                        if(strcmp(value, sec->title) == 0)
                            val = opt->values[i];
                    }
                }
                if(val && is_set(CFGF_NO_TITLE_DUPES, opt->flags))
                {
                    cfg_error(cfg, _("found duplicate title '%s'"), value);
                    return 0;
                }
            }
            if(val == NULL)
                val = _cfg_addval(opt);
        }
        else
            val = opt->values[0];
    }

    switch(opt->type)
    {
        case CFGT_INT:
            if(opt->parsecb)
            {
                if((*opt->parsecb)(cfg, opt, value, &i) != 0)
                    return 0;
                val->number = i;
            }
            else
            {
                val->number = strtol(value, &endptr, 0);
                if(*endptr != '\0')
                {
                    cfg_error(cfg, _("invalid integer value for option '%s'"),
                              opt->name);
                    return 0;
                }
                if(errno == ERANGE)
                {
                    cfg_error(cfg,
                          _("integer value for option '%s' is out of range"),
                              opt->name);
                    return 0;
                }
            }
            break;

        case CFGT_FLOAT:
            if(opt->parsecb)
            {
                if((*opt->parsecb)(cfg, opt, value, &f) != 0)
                    return 0;
                val->fpnumber = f;
            }
            else
            {
                val->fpnumber = strtod(value, &endptr);
                if(*endptr != '\0')
                {
                    cfg_error(cfg,
                          _("invalid floating point value for option '%s'"),
                              opt->name);
                    return 0;
                }
                if(errno == ERANGE)
                {
                    cfg_error(cfg,
                  _("floating point value for option '%s' is out of range"),
                              opt->name);
                    return 0;
                }
            }
            break;

        case CFGT_STR:
            free(val->string);
            if(opt->parsecb)
            {
                s = 0;
                if((*opt->parsecb)(cfg, opt, value, &s) != 0)
                    return 0;
                val->string = strdup(s);
            }
            else
                val->string = strdup(value);
            break;

        case CFGT_SEC:
            if(is_set(CFGF_MULTI, opt->flags) || val->section == 0)
            {
                cfg_free(val->section);
                val->section = calloc(1, sizeof(cfg_t));
                assert(val->section);
                val->section->name = strdup(opt->name);
                val->section->opts = cfg_dupopt_array(opt->subopts);
                val->section->flags = cfg->flags;
                val->section->filename = cfg->filename ? strdup(cfg->filename) : 0;
                val->section->line = cfg->line;
                val->section->errfunc = cfg->errfunc;
                val->section->title = value;
            }
            if(!is_set(CFGF_DEFINIT, opt->flags))
                cfg_init_defaults(val->section);
            break;

        case CFGT_BOOL:
            if(opt->parsecb)
            {
                if((*opt->parsecb)(cfg, opt, value, &b) != 0)
                    return 0;
            }
            else
            {
                b = cfg_parse_boolean(value);
                if(b == -1)
                {
                    cfg_error(cfg, _("invalid boolean value for option '%s'"),
                              opt->name);
                    return 0;
                }
            }
            val->boolean = (cfg_bool_t)b;
            break;

        case CFGT_PTR:
            assert(opt->parsecb);
            if((*opt->parsecb)(cfg, opt, value, &p) != 0)
                return 0;
            val->ptr = p;
            break;

        default:
            cfg_error(cfg, "internal error in cfg_setopt(%s, %s)",
                      opt->name, value);
            assert(0);
            break;
    }
    return val;
}

int _call_function(cfg_t *cfg, cfg_opt_t *opt, cfg_opt_t *funcopt)
{
    int ret;
    const char **argv;
    unsigned int i;

    /* create a regular argv string vector and call
     * the registered function
     */
    argv = calloc(funcopt->nvalues, sizeof(char *));
    for(i = 0; i < funcopt->nvalues; i++)
        argv[i] = funcopt->values[i]->string;
    ret = (*opt->func)(cfg, opt, funcopt->nvalues, argv);
    cfg_free_value(funcopt);
    free(argv);
    return ret;
}

DLLIMPORT void cfg_free_value(cfg_opt_t *opt)
{
    if(opt == 0)
        return;

    if(opt->values)
    {
        unsigned int i;

        for(i = 0; i < opt->nvalues; i++)
        {
            if(opt->type == CFGT_STR)
                free(opt->values[i]->string);
            else if(opt->type == CFGT_SEC)
                cfg_free(opt->values[i]->section);
            else if(opt->type == CFGT_PTR && opt->freecb && opt->values[i]->ptr)
                (opt->freecb)(opt->values[i]->ptr);
            free(opt->values[i]);
        }
        free(opt->values);
    }
    opt->values = 0;
    opt->nvalues = 0;
}

void cfg_free_opt_array(cfg_opt_t *opts)
{
    int i;

    for(i = 0; opts[i].name; ++i)
    {
        free(opts[i].name);
        if(opts[i].type == CFGT_FUNC || is_set(CFGF_LIST, opts[i].flags))
            free(opts[i].def.parsed);
        else if(opts[i].type == CFGT_STR)
            free(opts[i].def.string);
        else if(opts[i].type == CFGT_SEC)
            cfg_free_opt_array(opts[i].subopts);
    }
    free(opts);
}

DLLIMPORT int cfg_include(cfg_t *cfg, cfg_opt_t *opt, int argc,
                          const char **argv)
{
    opt = NULL;
    if(argc != 1)
    {
        cfg_error(cfg, _("wrong number of arguments to cfg_include()"));
        return 1;
    }
    return cfg_lexer_include(cfg, argv[0]);
}

cfg_value_t *cfg_opt_getval(cfg_opt_t *opt, unsigned int index)
{
    cfg_value_t *val = 0;

    assert(index == 0 || is_set(CFGF_LIST, opt->flags));

    if(opt->simple_value.ptr)
        val = (cfg_value_t *)opt->simple_value.ptr;
    else
    {
        if(is_set(CFGF_RESET, opt->flags))
        {
            cfg_free_value(opt);
            opt->flags &= ~CFGF_RESET;
        }

        if(index >= opt->nvalues) {
            val = _cfg_addval(opt);
	} else {
            val = opt->values[index];
	}
    }
    return val;
}

DLLIMPORT void cfg_opt_setnint(cfg_opt_t *opt, long int value,
                               unsigned int index)
{
    cfg_value_t *val;
    assert(opt && opt->type == CFGT_INT);
    val = cfg_opt_getval(opt, index);
    val->number = value;
}

DLLIMPORT void cfg_opt_setnfloat(cfg_opt_t *opt, double value,
                                 unsigned int index)
{
    cfg_value_t *val;
    assert(opt && opt->type == CFGT_FLOAT);
    val = cfg_opt_getval(opt, index);
    val->fpnumber = value;
}

DLLIMPORT void cfg_opt_setnbool(cfg_opt_t *opt, cfg_bool_t value,
                                unsigned int index)
{
    cfg_value_t *val;
    assert(opt && opt->type == CFGT_BOOL);
    val = cfg_opt_getval(opt, index);
    val->boolean = value;
}


DLLIMPORT void cfg_opt_setnstr(cfg_opt_t *opt, const char *value,
                               unsigned int index)
{
    cfg_value_t *val;
    assert(opt && opt->type == CFGT_STR);
    val = cfg_opt_getval(opt, index);
    free(val->string);
    val->string = value ? strdup(value) : 0;
}

void _cfg_addlist_internal(cfg_opt_t *opt,
			  unsigned int nvalues, va_list ap)
{
    unsigned int i;

    for(i = 0; i < nvalues; i++)
    {
        switch(opt->type)
        {
            case CFGT_INT:
                cfg_opt_setnint(opt, va_arg(ap, int), opt->nvalues);
                break;
            case CFGT_FLOAT:
                cfg_opt_setnfloat(opt, va_arg(ap, double),
                                  opt->nvalues);
                break;
            case CFGT_BOOL:
                cfg_opt_setnbool(opt, va_arg(ap, cfg_bool_t),
                                 opt->nvalues);
                break;
            case CFGT_STR:
                cfg_opt_setnstr(opt, va_arg(ap, char*), opt->nvalues);
                break;
            case CFGT_FUNC:
            case CFGT_SEC:
            default:
                break;
        }
    }
}

DLLIMPORT void cfg_opt_nprint_var(cfg_opt_t *opt, unsigned int index, FILE *fp)
{
    const char *str;

    assert(opt && fp);
    switch(opt->type)
    {
        case CFGT_INT:
            fprintf(fp, "%ld", cfg_opt_getnint(opt, index));
            break;
        case CFGT_FLOAT:
            fprintf(fp, "%f", cfg_opt_getnfloat(opt, index));
            break;
        case CFGT_STR:
            str = cfg_opt_getnstr(opt, index);
            fprintf(fp, "\"");
            while (str && *str)
	    {
                if(*str == '"')
                    fprintf(fp, "\\\"");
                else if(*str == '\\')
                    fprintf(fp, "\\\\");
                else
                    fprintf(fp, "%c", *str);
                str++;
            }
            fprintf(fp, "\"");
            break;
        case CFGT_BOOL:
            fprintf(fp, "%s", cfg_opt_getnbool(opt, index) ? "true" : "false");
            break;
        case CFGT_NONE:
        case CFGT_SEC:
        case CFGT_FUNC:
        case CFGT_PTR:
            break;
    }
}


DLLIMPORT void cfg_opt_print_indent(cfg_opt_t *opt, FILE *fp, int indent)
{
    assert(opt && fp);

    if(opt->type == CFGT_SEC)
    {
        cfg_t *sec;
        unsigned int i;

        for(i = 0; i < cfg_opt_size(opt); i++)
        {
            sec = cfg_opt_getnsec(opt, i);
            cfg_indent(fp, indent);
            if(is_set(CFGF_TITLE, opt->flags))
                fprintf(fp, "%s \"%s\" {\n", opt->name, cfg_title(sec));
            else
                fprintf(fp, "%s {\n", opt->name);
            cfg_print_indent(sec, fp, indent + 1);
            cfg_indent(fp, indent);
            fprintf(fp, "}\n");
        }
    }
    else if(opt->type != CFGT_FUNC && opt->type != CFGT_NONE)
    {
        if(is_set(CFGF_LIST, opt->flags))
        {
            cfg_indent(fp, indent);
            fprintf(fp, "%s = {", opt->name);

            if(opt->nvalues)
            {
                unsigned int i;

                if(opt->pf)
                    opt->pf(opt, 0, fp);
                else
                    cfg_opt_nprint_var(opt, 0, fp);
                for(i = 1; i < opt->nvalues; i++)
                {
                    fprintf(fp, ", ");
                    if(opt->pf)
                        opt->pf(opt, i, fp);
                    else
                        cfg_opt_nprint_var(opt, i, fp);
                }
            }

            fprintf(fp, "}");
        }
        else
        {
            cfg_indent(fp, indent);
            /* comment out the option if is not set */
            if(opt->simple_value.ptr)
            {
                if(opt->type == CFGT_STR && *opt->simple_value.string == 0)
                    fprintf(fp, "# ");
            }
            else
            {
                if(cfg_opt_size(opt) == 0 || (
                       opt->type == CFGT_STR && (opt->values[0]->string == 0 ||
                                             opt->values[0]->string[0] == 0)))
                    fprintf(fp, "# ");
            }
            fprintf(fp, "%s = ", opt->name);
            if(opt->pf)
                opt->pf(opt, 0, fp);
            else
                cfg_opt_nprint_var(opt, 0, fp);
        }
    
        fprintf(fp, "\n");
    }
    else if(opt->pf)
    {
        cfg_indent(fp, indent);
        opt->pf(opt, 0, fp);
        fprintf(fp, "\n");
    }
}

DLLIMPORT void cfg_opt_print(cfg_opt_t *opt, FILE *fp)
{
    cfg_opt_print_indent(opt, fp, 0);
}


DLLIMPORT cfg_print_func_t cfg_opt_set_print_func(cfg_opt_t *opt,
                                                  cfg_print_func_t pf)
{
    cfg_print_func_t oldpf;

    assert(opt);
    oldpf = opt->pf;
    opt->pf = pf;

    return oldpf;
}

cfg_opt_t *cfg_getopt_array(cfg_opt_t *rootopts, int cfg_flags, const char *name)
{
    unsigned int i;
    cfg_opt_t *opts = rootopts;

    assert(rootopts && name);

    while(name && *name)
    {
        cfg_t *seccfg;
        char *secname;
        size_t len = strcspn(name, "|");
        if(name[len] == 0 /*len == strlen(name)*/)
            /* no more subsections */
            break;
        if(len)
        {
            cfg_opt_t *secopt;
            secname = strndup(name, len);
            secopt = cfg_getopt_array(opts, cfg_flags, secname);
            free(secname);
            if(secopt == 0)
            {
                /*fprintf(stderr, "section not found\n");*/
                return 0;
            }
            if(secopt->type != CFGT_SEC)
            {
                /*fprintf(stderr, "not a section!\n");*/
                return 0;
            }

            if(!is_set(CFGF_MULTI, secopt->flags) &&
	       (seccfg = cfg_opt_getnsec(secopt, 0)) != 0)
	    {
                opts = seccfg->opts;
	    }
            else
                opts = secopt->subopts;
            if(opts == 0)
            {
                /*fprintf(stderr, "section have no subopts!?\n");*/
                return 0;
            }
        }
        name += len;
        name += strspn(name, "|");
    }

    for(i = 0; opts[i].name; i++)
    {
        if(is_set(CFGF_NOCASE, cfg_flags))
        {
            if(strcasecmp(opts[i].name, name) == 0)
                return &opts[i];
        }
        else
        {
            if(strcmp(opts[i].name, name) == 0)
                return &opts[i];
        }
    }
    return 0;
}

