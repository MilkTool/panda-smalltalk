
#include "st-types.h"
#include "st-compiler.h"
#include "st-universe.h"
#include "st-hashed-collection.h"
#include "st-symbol.h"
#include "st-object.h"
#include "st-class.h"
#include "st-context.h"
#include "st-primitives.h"
#include "st-compiled-method.h"
#include "st-byte-array.h"
#include "st-array.h"
#include "st-association.h"

/* 3 + 4 ! */
static st_oop
create_doIt_method (void)
{
    STError *error = NULL;
    
    static const char string1[] = 
	"doIt"
	"    | block |"
	"    block := [ :x | x * self ]."
	" ^  block value: 10";

    static const char string2[] = 
	"increment"
	"    ^ self + 1";

    st_compile_string (st_undefined_object_class, string1, &error);
    g_assert (error == NULL);

    st_compile_string (st_smi_class, string2, &error);
    g_assert (error == NULL);

    return st_dictionary_at (st_behavior_method_dictionary (st_undefined_object_class), st_symbol_new ("doIt"));
}

static st_oop
lookup_method (st_oop class, st_oop selector)
{
    st_oop dict;
    st_oop method = st_nil;

    while (method == st_nil) {

	dict = st_behavior_method_dictionary (class);
	
	method = st_dictionary_at (dict, selector);
	
	class = st_behavior_superclass (class);
	if (class == st_nil)
	    break;
    }

    return method;
}


static st_oop
send_unary_message (st_oop sender,
		    st_oop receiver,
		    st_oop selector)
{
    st_oop context;
    st_oop method;

    method = lookup_method (receiver, selector);
    g_assert (method != st_nil); 

    context = st_method_context_new (method);


    ST_CONTEXT_PART_SENDER (context) = sender;
    ST_CONTEXT_PART_IP (context) = st_smi_new (0);
    ST_CONTEXT_PART_SP (context) = st_smi_new (0);

    ST_METHOD_CONTEXT_RECEIVER (context) = receiver;
    ST_METHOD_CONTEXT_METHOD (context) = method;

    return context;
}

INLINE void
message_not_understand (STExecutionState *es, st_oop selector, guint argcount)
{
    st_oop message;

    message = st_message_new (selector, es->stack + es->sp + 1, argcount);
   
}



INLINE st_oop
new_context (STExecutionState *es,
	     st_oop  sender,
	     st_oop  receiver,
	     st_oop  method,
	     guint   argcount)
{
    st_oop context;

    context = st_method_context_new (method);

    /* transfer arguments to context */
    st_oop *arguments = st_method_context_temporary_frame (context);
    for (guint i = 0; i < argcount; i++)
	arguments[i] = es->stack[es->sp - argcount + i];

    ST_CONTEXT_PART_SENDER (context) = sender;
    ST_CONTEXT_PART_IP (context) = st_smi_new (0);
    ST_CONTEXT_PART_SP (context) = st_smi_new (0);
    ST_METHOD_CONTEXT_RECEIVER (context) = receiver;
    ST_METHOD_CONTEXT_METHOD (context) = method;

    es->sp -= argcount + 1;

    return context;
}

void
st_interpreter_set_active_context (STExecutionState *es,
				   st_oop  context)
{
    st_oop home;

    /* save executation state of active context */
    if (es->context != st_nil) {
	ST_CONTEXT_PART_IP (es->context) = st_smi_new (es->ip);
	ST_CONTEXT_PART_SP (es->context) = st_smi_new (es->sp);
    }
    
    if (st_object_class (context) == st_block_context_class) {

	home = ST_BLOCK_CONTEXT_HOME (context);
	
	es->method   = ST_METHOD_CONTEXT_METHOD (home);
	es->receiver = ST_METHOD_CONTEXT_RECEIVER (home);
	es->literals = st_array_element (st_compiled_method_literals (es->method), 1);
	es->temps    = st_method_context_temporary_frame (home);
	es->stack    = ST_BLOCK_CONTEXT_STACK (context);
    } else {
	es->method   = ST_METHOD_CONTEXT_METHOD (context);
	es->receiver = ST_METHOD_CONTEXT_RECEIVER (context);
	es->literals = st_array_element (st_compiled_method_literals (es->method), 1);
	es->temps    = st_method_context_temporary_frame (context);
	es->stack    = st_method_context_stack_frame (context);
    }

    es->context  = context;
    es->sp       = st_smi_value (ST_CONTEXT_PART_SP (context));
    es->ip       = st_smi_value (ST_CONTEXT_PART_IP (context));
    es->bytecode = st_compiled_method_code (es->method);
}

INLINE guchar *
set_active_context (STExecutionState *es, guchar *ip, st_oop context)
{
    es->ip = ip - es->bytecode;

    st_interpreter_set_active_context (es, context);
    
    return es->bytecode + es->ip;
} 

INLINE st_oop
send_does_not_understand (STExecutionState *es,
			  st_oop            receiver,
			  st_oop            selector,
			  guint             argcount)
{
    st_oop message;
    st_oop method;
    st_oop method_selector;
    
    method_selector = st_symbol_new ("doesNotUnderstand:");
    
    method = lookup_method (st_object_class (receiver), method_selector);
    g_assert (method != st_nil);
    
    message = st_message_new (method_selector,
			      es->stack + es->sp - argcount,
			      argcount);    

    /* pop arguments off stack and replace them with a Message object */
    es->sp -= argcount;
    ST_STACK_PUSH (es, message);
    
    return new_context (es, es->context,
			receiver, method, 1);
}

INLINE void
execute_primitive (STExecutionState *es,
		   guint    pindex,
		   guchar  *ip,
                   guchar **ip_ret)
{
    es->success = true;

    es->ip = ip - es->bytecode;

    st_primitives[pindex].func (es);

    *ip_ret = es->bytecode + es->ip;
}


#define SEND_TEMPLATE(es)						\
    st_oop method;							\
    st_oop context;							\
    guint  prim;							\
    guchar *ip_ret = NULL;						\
    STCompiledMethodFlags flags;					\
    									\
    ip += 1;								\
    									\
    method = lookup_method (st_object_class (es->msg_receiver), es->msg_selector);	\
									\
    if (method == st_nil) {						\
	context = send_does_not_understand (es, es->msg_receiver, es->msg_selector, es->msg_argcount); \
	ip = set_active_context (es, ip, context);			\
	break;								\
    }									\
									\
    flags = st_compiled_method_flags (method);				\
    if (flags == ST_COMPILED_METHOD_PRIMITIVE) {			\
    									\
	prim = st_compiled_method_primitive_index (method);		\
									\
	execute_primitive (es, prim, ip, &ip_ret);			\
									\
        if (es->success) {						\
	    ip = ip_ret;						\
            break;							\
        }								\
    }									\
    									\
    context = new_context (es, es->context,				\
			   es->msg_receiver, method,		       	\
			   es->msg_argcount);			       	\
    									\
    ip = set_active_context (es, ip, context)


static st_oop
interpreter_loop (STExecutionState *es)
{
    register guchar *ip = st_compiled_method_code (es->method);
    
    for (;;) {

	g_debug ("%i", *ip);

	switch (*ip) {
	    
	case STORE_POP_TEMP:
	    
	    es->temps[ip[1]] = ST_STACK_POP (es);
	    
	    ip += 2;
	    break;
	    
	case STORE_TEMP:
	    
	     es->temps[ip[1]] = ST_STACK_PEEK (es);
	     
	     ip += 2;
	     break;
	     
	case PUSH_TEMP:
	    
	    ST_STACK_PUSH (es, es->temps[ip[1]]);
	    
	    ip += 2;
	    break;

	case PUSH_SELF:
	    
	    ST_STACK_PUSH (es, es->receiver);
	    
	    ip += 1;
	    break;
	    
	case PUSH_TRUE:
	    
	    ST_STACK_PUSH (es, st_true);

	    ip += 1;
	    break;
	    
	case PUSH_FALSE:
	    
	    ST_STACK_PUSH (es, st_false);

	    ip += 1;
	    break;
	    
	case PUSH_NIL:
	    
	     ST_STACK_PUSH (es, st_nil);
	     
	     ip += 1;
	     break;
	     
	case PUSH_LITERAL_CONST:
	    
	    ST_STACK_PUSH (es, es->literals[ip[1]]);
	    
	    ip += 2;
	    break;
	    
	case JUMP_TRUE:
	    
	    if (ST_STACK_POP (es) == st_true)
		ip += 3 + ((ip[1] << 8) | ip[2]);
	    else
		ip += 3;
	    
	     break;
	     
	case JUMP_FALSE:
	    
	    if (ST_STACK_POP (es) == st_false)
		ip += 3 + ((ip[1] << 8) | ip[2]);
	    else
		ip += 3;
	    
	    break;
	    
	case JUMP:
	{
	    short offset = (ip[1] << 8) | ip[2];
	    
	     ip += ((offset >= 0) ? 3 : 0) + offset;
	     break;
	}
	
	case PUSH_LITERAL_VAR:
	{
	     st_oop var;
	     
	     var = st_association_value (es->literals[ip[1]]);
	     
	     ST_STACK_PUSH (es, var);
	     
	     ip += 2;
	     break;
	}
	
	case SEND_PLUS:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_PLUS];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	    
	    break;
	}

	case SEND_MINUS:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_MINUS];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	}
	
	case SEND_MUL:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_MUL];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	}
	
	case SEND_LT:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_LT];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	}
	
	case SEND_GT:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_GT];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);

	    break;
	}
	
	case SEND_LE:
	{   
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_LE];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);

	    break;
	}
	
	case SEND_GE:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_GE];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];

	    SEND_TEMPLATE (es);
	    
	    break;
	}
	
	case SEND_CLASS:
	{
	    es->msg_argcount = 0;
	    es->msg_selector = st_specials[ST_SPECIAL_CLASS];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	 }
	
	case SEND_AT:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_AT];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	 }
	
	case SEND_AT_PUT:
	{
	    es->msg_argcount = 2;
	    es->msg_selector = st_specials[ST_SPECIAL_ATPUT];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	}
	
	case SEND_IDENTITY_EQ:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_IDEQ];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	}

	case SEND_VALUE:
	{
	    es->msg_argcount = 0;
	    es->msg_selector = st_specials[ST_SPECIAL_VALUE];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	}
	
	case SEND_VALUE_ARG:
	{
	    es->msg_argcount = 1;
	    es->msg_selector = st_specials[ST_SPECIAL_VALUE_ARG];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    SEND_TEMPLATE (es);
	    
	    break;
	}

	case SEND:
	{
	    st_oop method;
	    st_oop context;
	    guint  pindex;
	    guchar *ip_ret = NULL;
	    STCompiledMethodFlags flags;
	    
	    es->msg_argcount = ip[1];
	    es->msg_selector = es->literals[ip[2]];
	    es->msg_receiver = es->stack[es->sp - es->msg_argcount - 1];
	    
	    ip += 3;
	    
	    method = lookup_method (st_object_class (es->msg_receiver), es->msg_selector);
	    
	    if (method == st_nil) {
		context = send_does_not_understand (es,
						    es->msg_receiver,
						    es->msg_selector,
						    es->msg_argcount);
		ip = set_active_context (es, ip, context);
		break;
	    }
	    
	    /* call primitive function if there is one */
	    flags = st_compiled_method_flags (method);

	    if (flags == ST_COMPILED_METHOD_PRIMITIVE) {
		pindex = st_compiled_method_primitive_index (method);
		
		execute_primitive (es, pindex, ip, &ip_ret);
		if (es->success) {
		    ip = ip_ret;
		    break;
		}
	    }
	    
	    context = new_context (es, es->context,
				   es->msg_receiver, method,
				   es->msg_argcount);
	    
	    ip = set_active_context (es, ip, context);
	    
	    break;
	}
	 
	case POP_STACK_TOP:
	    
	    (void) ST_STACK_POP (es);
	    
	    ip += 1;
	    break;
	    
	case BLOCK_COPY:
	{
	    st_oop block;
	    guint argcount = ip[1];
	    guint initial_ip;
	    
	    ip += 2;
	    
	    initial_ip = ip - es->bytecode + 3;

	    block = st_block_context_new (es->context, initial_ip, argcount);

	    ST_STACK_PUSH (es, block);
	    
	    break;
	}
	
	case RETURN_STACK_TOP:
	{
	    st_oop sender;
	    st_oop value;
	    
	    sender = ST_CONTEXT_PART_SENDER (es->context);
	    value = ST_STACK_PEEK (es);
	    
	    /* exit loop if sender is nil */
	    if (sender == st_nil)
		return value;
	    
	    ip = set_active_context (es, ip, sender);
	    
	    /* push returned value onto stack */
	    ST_STACK_PUSH (es, value);
	    
	    break;
	}
	
	case BLOCK_RETURN:
	{
	    st_oop caller;
	    st_oop value;
	    
	    caller = ST_BLOCK_CONTEXT_CALLER (es->context);
	    value = ST_STACK_PEEK (es);

	    ip = set_active_context (es, ip, caller);

	    /* push returned value onto caller's stack */
	    ST_STACK_PUSH (es, value);

	    g_assert (es->context == caller);
	    
	    break;
	}
	
	default:
	    
	    g_assert_not_reached ();
	}
	
    }
    
    return st_nil;
}

void
st_interpreter_main (void)
{
    st_oop context;
    st_oop result;
    STExecutionState es;

    create_doIt_method ();

    context = send_unary_message (st_nil,
				  st_undefined_object_class,
				  st_symbol_new ("doIt"));
    es.context = st_nil;
    st_interpreter_set_active_context (&es, context);

    result = interpreter_loop (&es);
    g_assert (st_object_is_smi (result));

    printf ("result: %i\n", st_smi_value (result));
}