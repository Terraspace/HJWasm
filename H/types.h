
/* prototypes of TYPES.C */

#ifndef _TYPES_H_INCLUDED_
#define _TYPES_H_INCLUDED_

/* qualified_type us used for parsing a qualified type. */
struct qualified_type {
    int             size;
    struct asym     *symtype;
    enum memtype    mem_type;
    uint_8          is_ptr; /* contains level of indirection */
    uint_8          is_far;
    uint_8          Ofssize;
    enum memtype    ptr_memtype;
};

extern struct dsym *CurrStruct; /* start of current STRUCT list */

extern struct asym *CreateTypeSymbol( struct asym *, const char *, bool );
extern struct asym *SearchNameInStruct( const struct asym *, const char *, uint_32 *, int level );
//extern ret_code    EndstructDirective( int );
extern ret_code    GetQualifiedType( int *, struct asm_tok[], struct qualified_type * );
extern struct asym *CreateStructField( int, struct asm_tok[], const char *, enum memtype, struct asym *, uint_32 );
extern void        UpdateStructSize( struct asym * );
extern ret_code    SetStructCurrentOffset( int_32 );
extern ret_code    AlignInStruct( int );
extern void        TypesInit( void );
extern void        DeleteType( struct dsym * );
#endif
