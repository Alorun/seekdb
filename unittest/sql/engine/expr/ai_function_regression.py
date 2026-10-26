#!/usr/bin/env python3
"""Run SQL regressions against an isolated seekdb and ai_function_mock.py."""
import argparse
import json
import os
import subprocess
from pathlib import Path
from ai_function_mock import fingerprint


def literal(value):
    # Hex literals avoid SQL-mode-dependent backslash escaping.
    return "convert(0x%s using utf8mb4)" % value.encode('utf-8').hex()


def run(args):
    prefix = 'ai_reg_%d' % os.getpid()
    database = prefix
    models = []
    endpoints = []
    client = ['mysql', '--no-defaults', '--protocol=tcp', '-h', args.host, '-P', str(args.port),
              '-uroot', '--default-character-set=utf8mb4', '--batch', '--raw', '--skip-column-names']

    def sql(statement, fail=False, use_db=True):
        command = client + (['-D', database] if use_db else []) + ['-e', statement]
        result = subprocess.run(command, text=True, capture_output=True)
        if fail:
            assert result.returncode != 0, statement
            return
        assert result.returncode == 0, result.stderr + '\n' + statement
        return result.stdout.rstrip('\n')

    def events():
        path = Path(args.events)
        rows = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        return [row for row in rows if row['body'].get('model', '').startswith('protocol-regression-')]

    def model(kind, provider, path):
        name = prefix + '_' + str(len(models))
        endpoint = name + '_ep'
        sql('call dbms_ai_service.create_ai_model(%s,%s)' %
            (literal(name), literal(json.dumps({'type': kind, 'model_name': 'protocol-regression-model'}))))
        models.append(name)
        sql('call dbms_ai_service.create_ai_model_endpoint(%s,%s)' %
            (literal(endpoint), literal(json.dumps({'ai_model_name': name, 'scope': 'all',
             'url': args.mock_url + path, 'provider': provider, 'access_key': '' if provider.lower() == 'ollama' else 'mock-key'}))))
        endpoints.append(endpoint)
        return name, endpoint

    def alter(endpoint, **fields):
        sql('call dbms_ai_service.alter_ai_model_endpoint(%s,%s)' %
            (literal(endpoint), literal(json.dumps(fields))))

    sql('create database ' + database, use_db=False)
    try:
        prompt = json.loads(sql("select ai_prompt('{0}/{0}/{1}',json_array(1,true),json_object('k',2))"))
        assert prompt['args'] == [[1, True], {'k': 2}]
        nested = json.loads(sql("select ai_prompt('outer:{0}',ai_prompt('inner:{0}','你好'))"))
        assert nested['args'] == [{'template': 'inner:{0}', 'args': ['你好']}]
        sql('create table prompts(t longtext,a longtext)')
        sql("insert into prompts values('{0}/{0}',repeat('界',70000))")
        assert sql("select json_length(ai_prompt(t,a)),char_length(json_unquote(json_extract(ai_prompt(t,a),'$.args[0]'))) from prompts") == '2\t70000'
        complete, complete_ep = model('completion', 'openai', '/compatible-mode/v1/chat/completions')
        assert sql("select ai_complete(%s,'hello',null)" % literal(complete)) == 'hello'
        alter(complete_ep, request_model_name='protocol-regression-override')
        assert sql("select ai_complete(%s,'model-override')" % literal(complete)) == 'model-override'
        assert events()[-1]['body']['model'] == 'protocol-regression-override'
        alter(complete_ep, request_model_name='')

        assert sql("select ai_complete(%s,ai_prompt('outer:{0}',ai_prompt('inner:{0}','你好')))" % literal(complete)) == 'outer:inner:你好'
        sql('create table stored_prompts(p json)')
        sql("insert into stored_prompts values(ai_prompt('inner:{0}','你好'))")
        assert sql("select ai_complete(%s,ai_prompt('outer:{0}',p)) from stored_prompts" % literal(complete)) == 'outer:inner:你好'

        assert sql("select ai_complete(%s,ai_prompt('{0}/{0}','你好'),json_object('temperature',0.2,'max_tokens',16))" % literal(complete)) == '你好/你好'
        last = events()[-1]['body']
        assert last['temperature'] == 0.2 and last['max_tokens'] == 16
        assert sql('select char_length(ai_complete(%s,ai_prompt(t,a))) from prompts' % literal(complete)) == '140001'
        for config in ['null', '[]', '1', '{"max_tokens":1.5}']:
            sql("select ai_complete(%s,'x',%s)" % (literal(complete), literal(config)), fail=True)
        sql("select ai_complete(%s,ai_prompt('{999999999999999999999999}','x'))" % literal(complete), fail=True)
        for route in ['/empty/', '/malformed/', '/wrong-root/', '/http-error/']:
            alter(complete_ep, url=args.mock_url + route + 'chat/completions')
            sql("select ai_complete(%s,'x')" % literal(complete), fail=True)
        alter(complete_ep, url='http://127.0.0.1:1/chat/completions')
        sql("select ai_complete(%s,'x')" % literal(complete), fail=True)
        alter(complete_ep, url=args.mock_url + '/timeout/chat/completions')
        sql("set ob_query_timeout=100000; select ai_complete(%s,'x')" % literal(complete), fail=True)
        alter(complete_ep, url=args.mock_url + '/compatible-mode/v1/chat/completions')
        print('prompt and completion SQL: passed', flush=True)

        embed, embed_ep = model('dense_embedding', 'openai', '/compatible-mode/v1/embeddings')
        assert sql("select json_length(ai_embed(%s,'single'))" % literal(embed)) == '768'
        assert sql("select json_length(ai_embed(%s,'single',3))" % literal(embed)) == '3'
        for dimension in ['0', '-1', '1.5', '18446744073709551615', "'3'", 'null']:
            sql("select ai_embed(%s,'x',%s)" % (literal(embed), dimension), fail=True)
        for route in ['/bad-dimension/', '/wrong-root/', '/malformed/', '/http-error/']:
            alter(embed_ep, url=args.mock_url + route + 'embeddings')
            sql("select ai_embed(%s,'x',3)" % literal(embed), fail=True)
        alter(embed_ep, url=args.mock_url + '/compatible-mode/v1/embeddings')
        sql('create table inputs(id int primary key,content text,dim int,model varchar(128))')
        values = [(i, None if i % 7 == 1 else 'input-%d' % i) for i in range(1, 514)]
        sql('insert into inputs values ' + ','.join('(%d,%s,%d,%s)' %
            (i, 'null' if text is None else literal(text), 2 if i % 2 else 3, literal(embed)) for i, text in values))
        start = len(events())
        output = sql('select id,ai_embed(%s,content,2) from inputs order by id' % literal(embed))
        for line, (i, text) in zip(output.splitlines(), values, strict=True):
            row, vector = line.split('\t')
            assert int(row) == i
            assert vector == 'NULL' if text is None else json.loads(vector) == [fingerprint(text), 0.25]
        requests = events()[start:]
        counts = [len(x['body']['input']) for x in requests]
        assert max(counts) > 1, 'SQL evaluation did not issue a real multi-input request'
        assert sum(counts) == sum(text is not None for _, text in values)
        assert len(counts) > 1, 'expected multiple engine batches and a tail batch'
        print('embedding SQL batches:', counts, flush=True)
        start = len(events())
        lengths = sql('select id,json_length(ai_embed(%s,content,2)) from inputs order by id' % literal(embed))
        for line, (i, text) in zip(lengths.splitlines(), values, strict=True):
            assert line == '%d\t%s' % (i, 'NULL' if text is None else '2')
        wrapped_counts = [len(x['body']['input']) for x in events()[start:]]
        assert max(wrapped_counts) > 1, 'JSON_LENGTH prevented child batch evaluation'
        print('JSON_LENGTH embedding SQL batches:', wrapped_counts, flush=True)
        sql('create table json_rows(id int primary key,doc json,path varchar(32))')
        sql("insert into json_rows values(1,'[1,2]','$'),(2,null,'$'),(3,'{\"a\":[1,2,3]}','$.a')")
        assert sql('select id,json_length(doc),json_length(doc,path) from json_rows order by id') == '1\t2\t2\n2\tNULL\tNULL\n3\t1\t3'

        sql('create table mapping_rows(id int primary key,content text)')
        sql("insert into mapping_rows values(1,'A'),(2,null),(3,'B'),(4,'C')")
        start = len(events())
        mapping = sql('select id,ai_embed(%s,content,2) from mapping_rows order by id' % literal(embed))
        for line, text in zip(mapping.splitlines(), ['A', None, 'B', 'C'], strict=True):
            vector = line.split('\t')[1]
            assert vector == 'NULL' if text is None else json.loads(vector)[0] == fingerprint(text)
        assert events()[start]['body']['input'] == ['A', 'B', 'C']
        start = len(events())
        sql('select ai_embed(%s,content) from inputs where content is null' % literal(embed))
        assert len(events()) == start, 'all-NULL input issued requests'
        filtered = sql('select id,ai_embed(%s,content,2) from inputs where id > 5 and id < 20 order by id limit 4' % literal(embed))
        for line in filtered.splitlines():
            i, vector = line.split('\t')
            text = dict(values)[int(i)]
            assert vector == 'NULL' if text is None else json.loads(vector)[0] == fingerprint(text)
        dynamic = sql('select id,ai_embed(model,content,dim) from inputs where id <= 15 order by id')
        for line in dynamic.splitlines():
            i, vector = line.split('\t')
            i = int(i)
            if dict(values)[i] is not None:
                assert len(json.loads(vector)) == (2 if i % 2 else 3)
        alter(embed_ep, url=args.mock_url + '/duplicate-index/embeddings')
        sql('select ai_embed(%s,content,2) from inputs where id between 2 and 4' % literal(embed), fail=True)
        alter(embed_ep, url=args.mock_url + '/compatible-mode/v1/embeddings')
        alter(embed_ep, url=args.mock_url + '/fail-second/' + prefix + '/embeddings')
        sql('select ai_embed(model,content,dim) from inputs where id between 2 and 4', fail=True)
        alter(embed_ep, url=args.mock_url + '/compatible-mode/v1/embeddings')
        assert json.loads(sql("select ai_embed(%s,'after-failure',2)" % literal(embed)))[0] == fingerprint('after-failure')
        print('embedding NULL/filter/LIMIT/dynamic configuration/error SQL: passed', flush=True)

        rerank, _ = model('rerank', 'siliconflow', '/v1/rerank')
        documents = ['same-%d' % i for i in range(45)]
        ranked = json.loads(sql("select ai_rerank(%s,'same',%s,null)" %
            (literal(rerank), literal(json.dumps(documents)))))
        assert [x['index'] for x in ranked] == list(range(45))
        assert [x['document']['text'] for x in ranked] == documents
        documents = [{'id': str(i), 'body': 'same-%d' % i, 'extra': {'keep': i}} for i in range(45)]
        ranked = json.loads(sql("select ai_rerank(%s,'same',%s,'body')" %
            (literal(rerank), literal(json.dumps(documents)))))
        assert [x['id'] for x in ranked] == [str(i) for i in range(45)]
        assert [x['extra'] for x in ranked] == [x['extra'] for x in documents]
        print('rerank multi-request identity and stable ties: passed', flush=True)

        ollama, ollama_ep = model('completion', 'ollama', '/api/generate')
        assert sql("select ai_complete(%s,'hello')" % literal(ollama)) == 'hello'
        assert events()[-1]['has_authorization'] is False
        alter(ollama_ep, provider='Ollama')
        assert sql("select ai_complete(%s,'provider-only-alter')" % literal(ollama)) == 'provider-only-alter'
        alter(ollama_ep, provider='OPENAI', access_key='mock-key', url=args.mock_url + '/compatible-mode/v1/chat/completions')
        assert sql("select ai_complete(%s,'openai-after-alter')" % literal(ollama)) == 'openai-after-alter'
        assert events()[-1]['has_authorization'] is True
        alter(ollama_ep, provider='Ollama', access_key='', url=args.mock_url + '/api/generate')
        assert sql("select ai_complete(%s,'ollama-after-alter')" % literal(ollama)) == 'ollama-after-alter'
        ollama_embed, _ = model('dense_embedding', 'OLLAMA', '/api/embed')
        assert sql("select json_length(ai_embed(%s,'hello',3))" % literal(ollama_embed)) == '3'
        print('Ollama native protocols and provider changes: passed', flush=True)
    finally:
        for endpoint in reversed(endpoints):
            sql('call dbms_ai_service.drop_ai_model_endpoint(%s)' % literal(endpoint))
        for name in reversed(models):
            sql('call dbms_ai_service.drop_ai_model(%s)' % literal(name))
        sql('drop database ' + database, use_db=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=12881)
    parser.add_argument('--mock-url', default='http://127.0.0.1:8000')
    parser.add_argument('--events', required=True)
    run(parser.parse_args())
