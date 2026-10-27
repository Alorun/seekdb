#!/usr/bin/env python3
"""Native seekdb integration checks; requires an isolated running instance.

python3 unittest/sql/engine/basic/test_file_sql_integration.py --port 2887
No external SQL engine or Python database driver is used.
"""
import argparse
import base64
import pathlib
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=2887)
    parser.add_argument('--user', default='root')
    parser.add_argument('--socket', help='Unix socket required to change secure_file_priv')
    parser.add_argument('--mysql', default='mysql')
    parser.add_argument('--large-rows', type=int, default=0, help='Optional streaming and memory regression, e.g. 131072')
    parser.add_argument('--server-pid', type=int, help='Local seekdb PID used to sample RSS and file descriptors')
    args = parser.parse_args()
    if args.large_rows and (args.large_rows < 0 or not args.server_pid):
        parser.error('--large-rows requires a positive row count and --server-pid')
    command = [args.mysql, '-h127.0.0.1', f'-P{args.port}', f'-u{args.user}', '-N', '-B', '--raw']
    if args.socket:
        command = [args.mysql, '--skip-ssl', '--protocol=SOCKET', '-S' + args.socket, f'-u{args.user}', '-N', '-B', '--raw']
    checks = 0

    def run(sql, expected=None, error=False, enabled=True):
        nonlocal checks
        prefix = 'set session enable_file_sql=ON;' if enabled else ''
        result = subprocess.run(command + ['-e', prefix + sql], capture_output=True, text=True, timeout=40)
        if error:
            assert result.returncode != 0, f'expected failure: {sql}\n{result.stdout}'
            if expected is not None:
                assert expected in result.stderr, (sql, result.stderr, expected)
        else:
            assert result.returncode == 0, f'{sql}\n{result.stderr}'
            if expected is not None:
                assert result.stdout.rstrip("\n") == expected, (sql, result.stdout, expected)
        checks += 1
        return result.stdout

    root = pathlib.Path(__file__).resolve().parents[4]
    fixtures = root / 'tools/deploy/mysql_test/test_suite/file_sql/data'
    original = run('select @@global.secure_file_priv', enabled=False).strip()
    with tempfile.TemporaryDirectory(prefix='seekdb-file-sql-') as directory:
        data = pathlib.Path(directory)
        database = 'file_sql_regression_' + data.name[-8:]
        database_created = False
        for name in ['basic.csv', 'basic.jsonl']:
            shutil.copyfile(fixtures / name, data / name)
        (data / 'simple.parquet').write_bytes(base64.b64decode((fixtures / 'simple.parquet.b64').read_bytes()))
        multiple_groups = pathlib.Path(__file__).parent / 'data/file_sql_multiple_groups.parquet.b64'
        group_bytes = base64.b64decode(multiple_groups.read_bytes())
        (data / 'groups.parquet').write_bytes(group_bytes)
        corrupt_levels = bytearray(group_bytes)
        corrupt_levels[47] ^= 255  # Bad level length previously panicked inside Bytes::slice.
        (data / 'corrupt-levels.parquet').write_bytes(corrupt_levels)
        (data / 'right.jsonl').write_text('{"id":1,"v":"x"}\n{"id":1,"v":"y"}\n{"id":4,"v":"z"}\n')
        (data / 'late.jsonl').write_text('{"id":1}\n{"id":2,"late":true}\n{"id":"last","tail":null}\n')
        (data / 'noheader.csv').write_text('1|"a|b"\r\n2|""\r\n')
        (data / 'bad.csv').write_text('a,b\n1\n')
        (data / 'nested.jsonl').write_text('{"a":[]}\n')
        (data / 'empty.csv').write_text('')
        (data / 'header.csv').write_text('a,b\n')
        (data / 'outside').symlink_to('/etc/passwd')
        (data / 'broken.parquet').write_bytes(b'PAR1broken')
        csv = f"read_csv('{data}/basic.csv')"
        json = f"read_ndjson('{data}/basic.jsonl')"
        parquet = f"read_parquet('{data}/simple.parquet')"
        try:
            run(f"set global secure_file_priv='{data}'", enabled=False)
            run('select @@session.enable_file_sql', '0', enabled=False)
            run(f'select count(*) from {csv}', error=True, enabled=False)
            run(f'select count(*),sum(id),sum(active) from {csv}', '3\t6\t2')
            run(f'select id,name,score from {csv} where score>90 order by score desc limit 1', '1\tAlice\t98.5')
            run(f'select id+1 as next_id,upper(name) as n from {csv} where id=1', '2\tALICE')
            run(f'select active,count(*) n from {csv} group by active having count(*)>1', '1\t2')
            run(f'select count(*),sum(id) from {csv} where id<0', '0\tNULL')
            run(f'select count(*),count(score),sum(score) from {json}', '3\t2\t188.5')
            run(f'select count(*) from {csv} c inner join {json} j on c.id=j.id', '3')
            nested = f"select /*+ use_nl(c j) leading(c j) no_use_nl_materialization(j) */ count(*) from {csv} c join {json} j on c.id=j.id"
            run(nested, '3')
            nested_plan = run('explain ' + nested)
            assert 'NESTED-LOOP JOIN' in nested_plan and 'MATERIAL' not in nested_plan, nested_plan

            run(f"select c.id,r.v from {csv} c left join read_json('{data}/right.jsonl') r on c.id=r.id order by c.id,r.v", '1\tx\n1\ty\n2\tNULL\n3\tNULL')
            run(f"select count(*),count(r.v) from {csv} c left join read_json('{data}/right.jsonl') r on c.id=r.id", '4\t2')
            run(f'select c.id,p.c1 from {csv} c join {parquet} p on c.id=p.c1 order by c.id', '1\t1\n2\t2\n3\t3')
            run(f'select count(*),min(c1),max(c1),sum(c1) from {parquet}', '7\t0\t6\t21')
            run(f'select count(*) from {parquet}', '7')
            groups = f"read_parquet('{data}/groups.parquet')"
            run(f'select count(*),count(id),count(name),sum(id) from {groups}', '5\t4\t4\t13')
            run(f'select count(*) from {groups}', '5')
            run(f'select id,name from {groups} order by id', 'NULL\ttwo\n1\tone\n3\tNULL\n4\tfour\n5\tfive')
            run(f"select name from {groups} where name like 'f%' order by name", 'five\nfour')
            run(f'select sum(p.id) from {csv} c join {groups} p on c.id=p.id', '4')
            run(f"select * from read_parquet('{data}/corrupt-levels.parquet')", 'panic contained', error=True)
            run('select 1', '1', enabled=False)
            if args.server_pid:
                for descriptor in (pathlib.Path('/proc') / str(args.server_pid) / 'fd').iterdir():
                    try:
                        assert descriptor.readlink() != data / 'corrupt-levels.parquet', descriptor
                    except FileNotFoundError:
                        pass
            run(f"select column0,column1 from read_csv('{data}/noheader.csv',header=false,delimiter='|') order by column0", '1\ta|b\n2\t')
            run(f"select id,late,tail from read_json('{data}/late.jsonl') order by id", '1\tNULL\tNULL\n2\t1\tNULL\nlast\tNULL\tNULL')
            schema = run(f'describe select * from {csv}')
            assert 'id\tBIGINT\tNO' in schema and 'active\tBOOLEAN\tNO' in schema and 'day\tDATE\tYES' in schema, schema
            plan = run(f'explain select c.id,sum(p.c1) from {csv} c join {parquet} p on c.id=p.c1 group by c.id')
            assert 'FUNCTION_TABLE' in plan, plan
            run(f"select count(*) from glob('{data}/*.csv')", '5')
            # Multiple batches, multiline fields crossing the 64 KiB input
            # boundary, and distinct long values expose stale datum references.
            payload_length = 0
            with (data / 'batches.csv').open('w') as out:
                out.write('id,payload\n')
                for i in range(385):
                    payload = f'{i:04d}-' + 'x' * (70000 + i % 3) + '\nend'
                    payload_length += len(payload)
                    out.write(f'{i},"{payload}"\n')
            run(f"select count(*),count(distinct payload),sum(length(payload)),sum(case when substr(payload,1,4)=lpad(id,4,'0') then 1 else 0 end) from read_csv('{data}/batches.csv')",
                f'385\t385\t{payload_length}\t385')
            (data / 'unicode.csv').write_text('É,e\n1,2\n')
            run(f"select * from read_csv('{data}/unicode.csv')", '1\t2')
            (data / 'unicode.jsonl').write_text('{"É":1}\n{"e":2}\n')
            run(f"select * from read_json('{data}/unicode.jsonl') order by `É`", '1\n2')

            for call in [f"read_csv('{data}/bad.csv')", f"read_csv('{data}/empty.csv')", f"read_json('{data}/nested.jsonl')", f"read_parquet('{data}/broken.parquet')", f"read_csv('{data}/outside')", "read_csv('/etc/passwd')", f"read_csv('{data}/basic.csv',sample_size=10)", f"read_csv('{data}/basic.csv',header=true,header=false)", f"read_csv('{data}/basic.csv',unknown=1)", f"read_csv('{data}/basic.csv',header=1)"]:
                run(f'select * from {call}', error=True)
            run(f"select count(*) from read_csv('{data}/header.csv')", '0')
            run(f"copy (select id,name,score,note from {csv} order by id) to '{data}/export.csv' (format csv,header,nullstr 'NULL')")
            run(f"select id,name,score,replace(note,char(10),'<NL>') from read_csv('{data}/export.csv') order by id", '1\tAlice\t98.5\thello,<NL>world\n2\tBob\t87\tplain\n3\tChen\t90.25\tsay "hi"')
            before = (data / 'export.csv').read_bytes()
            run(f"copy (select 1) to '{data}/export.csv'", error=True)
            assert before == (data / 'export.csv').read_bytes()
            run(f"copy (select 1 as v union all select 2) to '{data}/union.csv' (header)")
            run(f"select sum(v) from read_csv('{data}/union.csv')", '3')
            run(f"set session ob_query_timeout=100000;copy (select sleep(1)) to '{data}/cancel.csv'", error=True)
            assert not (data / 'cancel.csv').exists()
            run(f"copy (select repeat('x',4500000),repeat('y',4500000)) to '{data}/over-limit.csv'", error=True)
            assert not (data / 'over-limit.csv').exists()
            assert not list(data.glob('.seekdb-copy-*'))
            run(f'create database {database}')
            database_created = True
            run(f"create table {database}.t(id int, name varchar(20));insert into {database}.t values(1,'native'),(2,NULL)")
            run(f"copy {database}.t to '{data}/table.csv' (header)")
            run(f"select id,name from read_csv('{data}/table.csv') order by id", '1\tnative\n2\tNULL')
            run(f'select sum(id),count(name) from {database}.t', '3\t1')
            run(f'create table {database}.read_csv(read_csv int);insert into {database}.read_csv values(7);select read_csv from {database}.read_csv', '7', enabled=False)
            run('select count(*) from table(generator(3))', '3', enabled=False)
            run('select sum(v),count(*),count(distinct v) from (select 1 v union all select 1 union all select 2) t', '4\t3\t2', enabled=False)
            run(f'drop database {database}')
            database_created = False
            run(f"prepare fs from 'select count(*) from {csv.replace(chr(39), chr(39)*2)}';execute fs;execute fs;deallocate prepare fs", '3\n3')
            if args.large_rows:
                large = data / 'large.csv'
                record = ('1,' + 'x' * 1024 + '\n').encode()
                with large.open('wb') as out:
                    out.write(b'id,payload\n')
                    remaining = args.large_rows
                    while remaining:
                        chunk_rows = min(remaining, 1024)
                        out.write(record * chunk_rows)
                        remaining -= chunk_rows
                process_dir = pathlib.Path('/proc') / str(args.server_pid)
                def rss():
                    return next(int(line.split()[1]) * 1024 for line in (process_dir / 'status').read_text().splitlines() if line.startswith('VmRSS:'))
                baseline = peak = rss()
                query = subprocess.Popen(command + ['-e', f"set enable_file_sql=on;set ob_query_timeout=60000000;select count(*),sum(id) from read_csv('{large}');"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                started = time.monotonic()
                while query.poll() is None:
                    peak = max(peak, rss())
                    time.sleep(.01)
                output, diagnostic = query.communicate()
                assert query.returncode == 0, diagnostic
                assert output.strip() == f'{args.large_rows}\t{args.large_rows}', output
                size = large.stat().st_size
                if size >= 64 * 1024 * 1024:
                    assert peak - baseline < size // 2, (baseline, peak, size)
                print(f'streaming bytes={size} rows={args.large_rows} seconds={time.monotonic()-started:.2f} rss_delta={peak-baseline}')
                run(f"select id from read_csv('{large}') limit 1", '1')
                run(f"set ob_query_timeout=1000;select count(*) from read_csv('{large}')", error=True)
                for descriptor in (process_dir / 'fd').iterdir():
                    try:
                        assert descriptor.readlink() != large, descriptor
                    except FileNotFoundError:
                        pass
                large.unlink()
            run(f"set global secure_file_priv='';select count(*) from {csv}", error=True)
            run("select 1", "1", enabled=False)
        finally:
            restore = 'NULL' if original == 'NULL' else "'" + original.replace("'", "''") + "'"
            run('set global secure_file_priv=' + restore, enabled=False)
            if database_created:
                run(f'drop database {database}', enabled=False)
    print(f'{checks} native SQL checks passed')


if __name__ == '__main__':
    main()
