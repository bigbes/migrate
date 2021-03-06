#include "tuple.h"

#include <assert.h>
#include <stdint.h>
#include <inttypes.h>

#include <tarantool/module.h>
#include <tarantool/lua.h>
#include <tarantool/lauxlib.h>
#include <tarantool/lualib.h>

#include <small/ibuf.h>

#include <tarantool/tnt.h>

#include "mpstream.h"
#include "xlog.h"

extern struct ibuf xlog_ibuf;
extern box_tuple_format_t *tuple_format;

static void
mmpstream_tarantool_err(void *error_ctx, const char *err, size_t errlen)
{
	say_error("%.*s", (int )errlen, err);
	struct lua_State *L = (struct lua_State *) error_ctx;
	luaL_error(L, err);
}

static void
lua_field_encode(struct lua_State *L, struct mpstream *stream, const char *data,
		 size_t size, enum field_t tp, bool throws)
{
/*
 * if (tp == F_FLD_STR && size == 4) {
 *		say_info("strange");
 *	}
 */
//	say_info("throws: %d, size: %d", (int)throws, (int)size);
	if (tp == F_FLD_NUM && size == 4) {
//		say_info("encoding as uint");
		mmpstream_encode_uint(stream, *((uint32_t *)data));
	} else if (tp == F_FLD_NUM && size == 8) {
//		say_info("encoding as uint");
		mmpstream_encode_uint(stream, *((uint64_t *)data));
	} else {
		if (tp == F_FLD_NUM && throws)
			luaL_error(L, "Cannot convert field '%.*s' to type NUM,"
				      " exptected len 4 or 8, got '%zd'",
				      size, data, size);
		mmpstream_encode_str(stream, data, size);
	}
}

static const char *box_cfg_error = "Cannot get tuple_format (maybe box isn't "
	"configured. box.cfg{} is needed)";

void
luatu_tuple_fields(struct lua_State *L, struct tnt_tuple *t,
		   struct space_def *def)
{
	struct ibuf *buf = &xlog_ibuf;
	if (!tuple_format) tuple_format = box_tuple_format_default();
	if (!tuple_format) luaL_error(L, box_cfg_error);

	ibuf_reset(buf);
	struct mpstream stream;
	mmpstream_init(&stream, buf,
		      ibuf_reserve_cb,
		      ibuf_alloc_cb,
		      mmpstream_tarantool_err, L);

	mmpstream_encode_array(&stream, t->cardinality);
	struct tnt_iter ifl;
	tnt_iter(&ifl, t);
	while (tnt_next(&ifl)) {
		int idx = TNT_IFIELD_IDX(&ifl);
		assert(idx < t->cardinality);
		char *data = TNT_IFIELD_DATA(&ifl);
		uint32_t size = TNT_IFIELD_SIZE(&ifl);
		enum field_t tp = F_FLD_STR;
		int throws = true;
		if (def) {
			tp = def->defaults;
			if (idx < def->schema_len)
				tp = def->schema[idx];
		}
		lua_field_encode(L, &stream, data, size, tp, throws);
	}
	if (ifl.status == TNT_ITER_FAIL)
		luaL_error(L, "failed to parse tuple");
	tnt_iter_free(&ifl);

	box_tuple_t *tuple = box_tuple_new(tuple_format, stream.buf, stream.pos);
	if (tuple == NULL)
		luaL_error(L, "%s: out of memory (box_tuple_new)", __func__);
	luaT_pushtuple(L, tuple);
	return;
}

void
luatu_key_fields(struct lua_State *L, struct tnt_tuple *t,
		 struct space_def *def)
{
	struct ibuf *buf = &xlog_ibuf;
	if (!tuple_format) tuple_format = box_tuple_format_default();
	if (!tuple_format) luaL_error(L, box_cfg_error);

	ibuf_reset(buf);
	struct mpstream stream;
	mmpstream_init(&stream, buf,
		      ibuf_reserve_cb,
		      ibuf_alloc_cb,
		      mmpstream_tarantool_err, L);

	mmpstream_encode_array(&stream, t->cardinality);
	struct tnt_iter ifl;
	tnt_iter(&ifl, t);
	while (tnt_next(&ifl)) {
		int idx = TNT_IFIELD_IDX(&ifl);
		assert(idx < t->cardinality);
		char *data = TNT_IFIELD_DATA(&ifl);
		uint32_t size = TNT_IFIELD_SIZE(&ifl);
		enum field_t tp = F_FLD_STR;
		int throws = true;
		if (def) {
			tp = def->defaults;
			if (idx < def->ischema_len)
				tp = def->ischema[idx];
		}
		lua_field_encode(L, &stream, data, size, tp, throws);
	}
	if (ifl.status == TNT_ITER_FAIL)
		luaL_error(L, "failed to parse tuple");
	tnt_iter_free(&ifl);

	box_tuple_t *tuple = box_tuple_new(tuple_format, stream.buf, stream.pos);

	if (tuple == NULL)
		luaL_error(L, "%s: out of memory (box_tuple_new)", __func__);
	luaT_pushtuple(L, tuple);
	return;
}

void
luatu_ops_fields(struct lua_State *L, struct tnt_request_update *req,
		 struct space_def *def)
{
	struct ibuf *buf = &xlog_ibuf;
	if (!tuple_format) tuple_format = box_tuple_format_default();
	if (!tuple_format) luaL_error(L, box_cfg_error);

	ibuf_reset(buf);
	struct mpstream stream;
	mmpstream_init(&stream, buf,
		      ibuf_reserve_cb,
		      ibuf_alloc_cb,
		      mmpstream_tarantool_err, L);

	uint32_t i = 0;
	mmpstream_encode_array(&stream, req->opc);
	for (i = 0; i < req->opc; ++i) {
		struct tnt_request_update_op *op = &req->opv[i];
		if (op->op >= TNT_UPDATE_MAX)
			luaL_error(L, "Undefined update operation: 0x%02x",
				   op->op);
		char *data = op->data;
		uint32_t size = op->size;
		mmpstream_encode_array(&stream, update_op_records[op->op].args_count);
		mmpstream_encode_str(&stream, update_op_records[op->op].operation, 1);
		mmpstream_encode_uint(&stream, op->field + 1);
		switch (op->op) {
		case TNT_UPDATE_ADD:
		case TNT_UPDATE_AND:
		case TNT_UPDATE_XOR:
		case TNT_UPDATE_OR: {
			lua_field_encode(L, &stream, data, size, F_FLD_NUM, true);
			break;
		}
		case TNT_UPDATE_INSERT:
		case TNT_UPDATE_ASSIGN: {
			enum field_t tp = F_FLD_STR;
			int throws = true;
			if (def) {
				tp = def->defaults;
				if (op->field < def->schema_len)
					tp = def->schema[op->field];
			}
			lua_field_encode(L, &stream, data, size, tp, throws);
			break;
		}
		case TNT_UPDATE_SPLICE: {
			size_t pos = 1;
			mmpstream_encode_uint(&stream, *(int32_t *)(data + pos));
			pos += 5;
			mmpstream_encode_uint(&stream, *(int32_t *)(data + pos));
			pos += 4 + op->size_enc_len;
			mmpstream_encode_str(&stream, data, size - pos);
			break;
		}
		case TNT_UPDATE_DELETE:
			mmpstream_encode_uint(&stream, 1);
			break;
		}
	}

	box_tuple_t *tuple = box_tuple_new(tuple_format, stream.buf, stream.pos);
	if (tuple == NULL)
		luaL_error(L, "%s: out of memory (box_tuple_new)", __func__);
	luaT_pushtuple(L, tuple);
	return;
}
