#define KISSDB_TEST

/* (Keep It) Simple Stupid Database
 *
 * Written by Adam Ierymenko <adam.ierymenko@zerotier.com>
 * KISSDB is in the public domain and is distributed with NO WARRANTY.
 *
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/* Compile with KISSDB_TEST to build as a test program. */
/* KISSDB_TEST를 사용하여 테스트 프로그램으로 빌드하시오 */

/* Note: big-endian systems will need changes to implement byte swapping
 * on hash table file I/O. Or you could just use it as-is if you don't care
 * that your database files will be unreadable on little-endian systems. */
/* 참고: big-endian 시스템에서는 해시 테이블 파일 입출력에 대한 바이트 스왑을 구현하기 위해 변경이 필요합니다.
 * 또는 리틀 엔디언 시스템에서 읽을 수 없는 데이터베이스 파일이 되더라도 신경쓰지 않으려면 그냥 사용할 수 있습니다. */


#define _FILE_OFFSET_BITS 64

#include "kissdb.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

#define KISSDB_HEADER_SIZE ((sizeof(uint64_t) * 3) + 4)

/* djb2 hash function */
/* djb2 해시 함수 */
static uint64_t KISSDB_hash(const void *b,unsigned long len)
{
	unsigned long i;
	uint64_t hash = 5381;
	for(i=0;i<len;++i)
		hash = ((hash << 5) + hash) + (uint64_t)(((const uint8_t *)b)[i]);
	return hash;
}


pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;


void access_database() {
    // 뮤텍스를 사용하여 데이터베이스에 대한 안전한 동시 액세스 보장
    pthread_mutex_lock(&db_mutex);
    
    // 데이터베이스 작업 수행 (work 함수 만들어서 실행)
    
    pthread_mutex_unlock(&db_mutex); // 뮤텍스 잠금 해제
}


/* KISSDB 데이터베이스를 열거나 생성(초기화)합니다. */
int KISSDB_open(
	KISSDB *db,
	const char *path,
	int mode,
	unsigned long hash_table_size,
	unsigned long key_size,
	unsigned long value_size)
{
	uint64_t tmp;
	uint8_t tmp2[4];
	uint64_t *httmp;
	uint64_t *hash_tables_rea;


    /* Windows용 플랫폼별 파일 I/O 처리 */
#ifdef _WIN32
	db->f = (FILE *)0;
	fopen_s(&db->f,path,((mode == KISSDB_OPEN_MODE_RWREPLACE) ? "w+b" : (((mode == KISSDB_OPEN_MODE_RDWR)||(mode == KISSDB_OPEN_MODE_RWCREAT)) ? "r+b" : "rb")));
#else
	db->f = fopen(path,((mode == KISSDB_OPEN_MODE_RWREPLACE) ? "w+b" : (((mode == KISSDB_OPEN_MODE_RDWR)||(mode == KISSDB_OPEN_MODE_RWCREAT)) ? "r+b" : "rb")));
#endif
	if (!db->f) {
		if (mode == KISSDB_OPEN_MODE_RWCREAT) {
#ifdef _WIN32
			db->f = (FILE *)0;
			fopen_s(&db->f,path,"w+b");
#else
			db->f = fopen(path,"w+b");
#endif
		}
		if (!db->f)
			return KISSDB_ERROR_IO;
	}


/* 데이터베이스 파일에 유효한 헤더가 있는지 확인합니다. */
/* 1. fseeko(db->f, 0, SEEK_END): 파일 포인터를 파일 끝으로 이동시킵니다. 이때 fseeko 함수는 파일 끝으로 이동하고, SEEK_END는 파일 끝을 나타냅니다.
   2. ftello(db->f) < KISSDB_HEADER_SIZE: 현재 파일 포인터의 위치를 ftello 함수로 얻어오고,
      이 위치가 KISSDB_HEADER_SIZE보다 작으면 헤더가 충분히 크지 않다고 판단합니다. */
	if (fseeko(db->f,0,SEEK_END)) {
		fclose(db->f);
		return KISSDB_ERROR_IO;
	}
	if (ftello(db->f) < KISSDB_HEADER_SIZE) {
		/* write header if not already present */
		/* 헤더가 없으면 헤더를 작성합니다. */
		if ((hash_table_size)&&(key_size)&&(value_size)) {  // hash_table_size, key_size, value_size가 참인지 확인
			if (fseeko(db->f,0,SEEK_SET)) { fclose(db->f); return KISSDB_ERROR_IO; } //파일 포인터를 파일의 시작 부분으로 이동시킵니다.
			tmp2[0] = 'K'; tmp2[1] = 'd'; tmp2[2] = 'B'; tmp2[3] = KISSDB_VERSION; 
			if (fwrite(tmp2,4,1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; } //헤더 식별자인 "KdBC"와 버전 정보를 파일에 씁니다.
			tmp = hash_table_size;
			if (fwrite(&tmp,sizeof(uint64_t),1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; } 
			tmp = key_size;
			if (fwrite(&tmp,sizeof(uint64_t),1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; }
			tmp = value_size;
			if (fwrite(&tmp,sizeof(uint64_t),1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; } //hash_table_size, key_size, value_size 값을 파일에 씁니다.
			fflush(db->f); //파일 버퍼를 비워서 파일에 쓴 내용을 실제 디스크에 반영합니다.
		} else {
			fclose(db->f);
			return KISSDB_ERROR_INVALID_PARAMETERS;
		}
	} else {
		/* 기존 헤더를 읽습니다. */
		if (fseeko(db->f,0,SEEK_SET)) { fclose(db->f); return KISSDB_ERROR_IO; }
		if (fread(tmp2,4,1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; }
		if ((tmp2[0] != 'K')||(tmp2[1] != 'd')||(tmp2[2] != 'B')||(tmp2[3] != KISSDB_VERSION)) {
			fclose(db->f);
			return KISSDB_ERROR_CORRUPT_DBFILE;
		}
		if (fread(&tmp,sizeof(uint64_t),1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; }
		if (!tmp) {
			fclose(db->f);
			return KISSDB_ERROR_CORRUPT_DBFILE;
		}
		hash_table_size = (unsigned long)tmp;
		if (fread(&tmp,sizeof(uint64_t),1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; }
		if (!tmp) {
			fclose(db->f);
			return KISSDB_ERROR_CORRUPT_DBFILE;
		}
		key_size = (unsigned long)tmp;
		if (fread(&tmp,sizeof(uint64_t),1,db->f) != 1) { fclose(db->f); return KISSDB_ERROR_IO; }
		if (!tmp) {
			fclose(db->f);
			return KISSDB_ERROR_CORRUPT_DBFILE;
		}
		value_size = (unsigned long)tmp;
	}

	db->hash_table_size = hash_table_size;
	db->key_size = key_size;
	db->value_size = value_size;
	db->hash_table_size_bytes = sizeof(uint64_t) * (hash_table_size + 1); /* [hash_table_size] == next table */

	httmp = malloc(db->hash_table_size_bytes);
	if (!httmp) {
		fclose(db->f);
		return KISSDB_ERROR_MALLOC;
	}
	db->num_hash_tables = 0;
	db->hash_tables = (uint64_t *)0;
	while (fread(httmp,db->hash_table_size_bytes,1,db->f) == 1) {
		hash_tables_rea = realloc(db->hash_tables,db->hash_table_size_bytes * (db->num_hash_tables + 1));
		if (!hash_tables_rea) {
			KISSDB_close(db);
			free(httmp);
			return KISSDB_ERROR_MALLOC;
		}
		db->hash_tables = hash_tables_rea;

		memcpy(((uint8_t *)db->hash_tables) + (db->hash_table_size_bytes * db->num_hash_tables),httmp,db->hash_table_size_bytes);
		++db->num_hash_tables;
		if (httmp[db->hash_table_size]) {
			if (fseeko(db->f,httmp[db->hash_table_size],SEEK_SET)) {
				KISSDB_close(db);
				free(httmp);
				return KISSDB_ERROR_IO;
			}
		} else break;
	}
	free(httmp);

	return 0;
}


/*
1. 파일의 처음으로 이동합니다. (fseeko(db->f, 0, SEEK_SET))
2. 파일에서 4바이트를 읽어와서 임시 배열 tmp2에 저장합니다. 이것이 헤더의 일부입니다. (fread(tmp2, 4, 1, db->f))
3. 읽어온 헤더가 유효한지 확인합니다. (if ((tmp2[0] != 'K') || (tmp2[1] != 'd') || (tmp2[2] != 'B') || (tmp2[3] != KISSDB_VERSION)))
4. 다음으로 8바이트를 읽어와서 tmp에 저장하고, 이 값이 0인지 확인합니다. (if (fread(&tmp, sizeof(uint64_t), 1, db->f) != 1 || !tmp))
5. 위의 과정을 세 번 반복하여 해시 테이블 크기, 키 크기, 값 크기를 읽어옵니다.
6. 읽어온 값들을 데이터베이스 구조체에 저장합니다.
*/




/* KISSDB 데이터베이스를 닫습니다. */
void KISSDB_close(KISSDB *db)
{
	if (db->hash_tables) 
		free(db->hash_tables); //데이터베이스 객체에 할당된 해시 테이블 메모리를 해제합니다. 이는 데이터베이스가 사용한 동적 메모리를 반환하여 메모리 누수를 방지합니다.
	if (db->f) 
		fclose(db->f); //데이터베이스 파일을 닫습니다. 이는 데이터베이스 파일에 대한 파일 핸들을 닫아 파일의 접근을 종료하는 것을 의미합니다.
	memset(db,0,sizeof(KISSDB)); //KISSDB 구조체를 0으로 초기화합니다. 이렇게 함으로써 데이터베이스 객체의 모든 필드를 초기화하고 재사용할 수 있는 상태로 만듭니다.
}


/* 주어진 키를 기반으로 KISSDB 데이터베이스에서 값을 검색합니다. */
int KISSDB_get(KISSDB *db,const void *key,void *vbuf)
{
	uint8_t tmp[4096];
	const uint8_t *kptr;
	unsigned long klen,i;
	uint64_t hash = KISSDB_hash(key,db->key_size) % (uint64_t)db->hash_table_size; //주어진 키에 대한 해시 값을 계산합니다. 이 해시 값은 데이터베이스의 해시 테이블에서 해당 키의 위치를 결정하는 데 사용됩니다.
	uint64_t offset; 
	uint64_t *cur_hash_table;
	long n;

	cur_hash_table = db->hash_tables;
	for(i=0;i<db->num_hash_tables;++i) {
		offset = cur_hash_table[hash]; //해시 값을 사용하여 해당하는 해시 테이블의 오프셋을 가져옵니다. 이 오프셋은 데이터베이스 파일에서 해당 키의 값이 저장된 위치를 가리킵니다.
		if (offset) { //만약 오프셋이 0이 아니라면 해당 키의 값이 존재하는 것으로 판단하여 이어서 처리합니다. 오프셋이 0이라면 해당 키의 값이 데이터베이스에 존재하지 않는 것으로 판단하고 함수를 종료합니다.
			if (fseeko(db->f,offset,SEEK_SET)) //데이터베이스 파일에서 오프셋 위치로 이동합니다. 이 때, 파일 이동에 실패하면 입출력 오류를 반환합니다.
				return KISSDB_ERROR_IO;

			kptr = (const uint8_t *)key;
			klen = db->key_size;
			while (klen) { //키와 데이터베이스 파일에서 읽은 값을 비교하여 일치하는지 확인합니다. 일치하지 않는다면 다음 해시 테이블로 넘어가서 다시 확인합니다.
				n = (long)fread(tmp,1,(klen > sizeof(tmp)) ? sizeof(tmp) : klen,db->f);
				if (n > 0) {
					if (memcmp(kptr,tmp,n)) 
						goto get_no_match_next_hash_table;
					kptr += n;
					klen -= (unsigned long)n;
				} else return 1; /* not found */
			}

			if (fread(vbuf,db->value_size,1,db->f) == 1) //키와 일치하는 값을 찾았을 때, 해당 값을 읽어와서 반환합니다. 데이터를 성공적으로 읽었다면 0을 반환하고, 그렇지 않으면 입출력 오류를 반환합니다.
				return 0; /* success */
			else return KISSDB_ERROR_IO;
		} else return 1; /* not found */
get_no_match_next_hash_table:
		cur_hash_table += db->hash_table_size + 1; //일치하는 값을 찾지 못하고 다음 해시 테이블로 이동합니다.
	}

	return 1; /* not found */ //마지막으로 모든 해시 테이블을 검색했음에도 불구하고 일치하는 값을 찾지 못했을 경우에는 1을 반환하여 해당 키의 값이 데이터베이스에 존재하지 않음을 나타냅니다.
}


/* KISSDB 데이터베이스에 키-값 쌍을 삽입하거나 업데이트합니다. */
int KISSDB_put(KISSDB *db,const void *key,const void *value) // 데이터베이스에 값을 추가하는 함수
{
	uint8_t tmp[4096];
	const uint8_t *kptr;
	unsigned long klen,i;
	uint64_t hash = KISSDB_hash(key,db->key_size) % (uint64_t)db->hash_table_size; //주어진 키를 해싱하여 해시값을 계산합니다.
	uint64_t offset;
	uint64_t htoffset,lasthtoffset;
	uint64_t endoffset;
	uint64_t *cur_hash_table;
	uint64_t *hash_tables_rea;
	long n;

	lasthtoffset = htoffset = KISSDB_HEADER_SIZE; //데이터베이스 파일에서의 해시 테이블 오프셋을 초기화합니다.
	cur_hash_table = db->hash_tables; //현재 해시 테이블에 대한 포인터를 설정합니다.
	for(i=0;i<db->num_hash_tables;++i) { //모든 해시 테이블을 반복하여 값을 찾습니다.
		offset = cur_hash_table[hash]; // 현재 해시 테이블에서 주어진 키의 해시값에 해당하는 위치를 가져옵니다.
		if (offset) { //만약 해당 위치에 값이 이미 존재하는 경우를 처리합니다.
			/* rewrite if already exists */
			if (fseeko(db->f,offset,SEEK_SET)) //이미 존재하는 값의 위치로 이동합니다.
				return KISSDB_ERROR_IO;

			kptr = (const uint8_t *)key;
			klen = db->key_size;
			while (klen) { //키를 읽어와서 주어진 키와 비교하여 일치하는지 확인합니다.
				n = (long)fread(tmp,1,(klen > sizeof(tmp)) ? sizeof(tmp) : klen,db->f);
				if (n > 0) {
					if (memcmp(kptr,tmp,n))
						goto put_no_match_next_hash_table; //키가 일치하지 않는 경우 다음 해시 테이블로 이동합니다.
					kptr += n;
					klen -= (unsigned long)n;
				}
			}

			/* C99 spec demands seek after fread(), required for Windows */
			/* C99 표준에서 fread 함수를 사용한 후 파일 위치 이동 함수(fseek)를 호출하는 것을 요구 */
			fseeko(db->f,0,SEEK_CUR); //(빈 해시 테이블을 찾기 위해)파일 끝으로 이동 
 
			if (fwrite(value,db->value_size,1,db->f) == 1) { //데이터를 파일에 기록하는 함수(비어있지 않을 경우 덮어씌움)
				fflush(db->f); // 파일 버퍼를 비우고 변경 사항을 디스크에 즉시 반영하는 함수
				return 0; /* success */
			} else return KISSDB_ERROR_IO; //파일 기록 작업이 실패했을 경우 에러
		} else {
			/* add if an empty hash table slot is discovered */
			/* 빈 해시 테이블 슬롯이 발견되면 추가 */
			if (fseeko(db->f,0,SEEK_END)) //파일 끝으로 이동
				return KISSDB_ERROR_IO; 
			endoffset = ftello(db->f); //파일의 현재 오프셋을 가져와서 endoffset에 저장

			if (fwrite(key,db->key_size,1,db->f) != 1) // 키와 값을 파일에 기록
				return KISSDB_ERROR_IO;
			if (fwrite(value,db->value_size,1,db->f) != 1) // 키와 값을 파일에 기록
				return KISSDB_ERROR_IO;

			if (fseeko(db->f,htoffset + (sizeof(uint64_t) * hash),SEEK_SET)) //해시 테이블 내에서 적절한 위치로 이동
				return KISSDB_ERROR_IO;
			if (fwrite(&endoffset,sizeof(uint64_t),1,db->f) != 1) //키의 해시에 해당하는 위치에 새로운 값의 오프셋을 저장
				return KISSDB_ERROR_IO;
			cur_hash_table[hash] = endoffset; //현재 해시 테이블의 항목을 업데이트

			fflush(db->f); //버퍼링된 데이터를 디스크로 플러시

			return 0; /* success */
		}
put_no_match_next_hash_table: //해시 충돌이 발생하고 다음 해시 테이블로 이동할 때 해당 위치를 가리키는 지점을 나타냄
		lasthtoffset = htoffset; //lasthtoffset를 현재 htoffset 값으로 설정합니다. 이전 해시 테이블의 오프셋을 저장합니다.
		htoffset = cur_hash_table[db->hash_table_size]; //htoffset를 다음 해시 테이블의 오프셋으로 설정합니다. 이 값은 현재 해시 테이블 다음에 오는 테이블의 오프셋을 가르킴
		cur_hash_table += (db->hash_table_size + 1); //cur_hash_table을 다음 해시 테이블로 이동합니다. 새로운 해시 테이블을 처리하기 위해 포인터를 이동합니다.
	}

	/* if no existing slots, add a new page of hash table entries */
	/* 기존 슬롯이 없는 경우 해시 테이블 항목의 새 페이지 추가 */
	if (fseeko(db->f,0,SEEK_END)) // 파일 끝으로 이동하여 새로운 해시 테이블 페이지이 추가될 위치를 찾음
		return KISSDB_ERROR_IO;
	endoffset = ftello(db->f);

	hash_tables_rea = realloc(db->hash_tables,db->hash_table_size_bytes * (db->num_hash_tables + 1)); //찾은 위치를 hash_tables_rea에 저장
	if (!hash_tables_rea)
		return KISSDB_ERROR_MALLOC;
	db->hash_tables = hash_tables_rea; //메모리 할당
	cur_hash_table = &(db->hash_tables[(db->hash_table_size + 1) * db->num_hash_tables]); //새로운 해시테이블 포인터 설정
	memset(cur_hash_table,0,db->hash_table_size_bytes); // 메모리 비움

	cur_hash_table[hash] = endoffset + db->hash_table_size_bytes; /* 새 메모리 연결 */

	if (fwrite(cur_hash_table,db->hash_table_size_bytes,1,db->f) != 1) //db->hash_table_size_bytes만큼의 데이터를 db->f 파일에 씀
		return KISSDB_ERROR_IO;

	if (fwrite(key,db->key_size,1,db->f) != 1) //key를 파일에 씀
		return KISSDB_ERROR_IO;
	if (fwrite(value,db->value_size,1,db->f) != 1) //value를 파일에 씀
		return KISSDB_ERROR_IO;

	if (db->num_hash_tables) { //이미 해시테이블이 존재한다면 오프셋 후 추가
		if (fseeko(db->f,lasthtoffset + (sizeof(uint64_t) * db->hash_table_size),SEEK_SET))
			return KISSDB_ERROR_IO;
		if (fwrite(&endoffset,sizeof(uint64_t),1,db->f) != 1)
			return KISSDB_ERROR_IO;
		db->hash_tables[((db->hash_table_size + 1) * (db->num_hash_tables - 1)) + db->hash_table_size] = endoffset;
	}

	++db->num_hash_tables;

	fflush(db->f);

	return 0; /* success */
}


/* 데이터베이스를 반복하는 데 사용할 KISSDB 이터레이터(내용 탐색 도구)를 초기화합니다. */
void KISSDB_Iterator_init(KISSDB *db,KISSDB_Iterator *dbi)
{
	dbi->db = db;
	dbi->h_no = 0;
	dbi->h_idx = 0;
}


/* KISSDB 이터레이터를 데이터베이스의 다음 키-값 쌍으로 이동시킵니다(순회한다). */
int KISSDB_Iterator_next(KISSDB_Iterator *dbi,void *kbuf,void *vbuf)
{
	uint64_t offset;

	if ((dbi->h_no < dbi->db->num_hash_tables)&&(dbi->h_idx < dbi->db->hash_table_size)) {
		while (!(offset = dbi->db->hash_tables[((dbi->db->hash_table_size + 1) * dbi->h_no) + dbi->h_idx])) {
			if (++dbi->h_idx >= dbi->db->hash_table_size) {
				dbi->h_idx = 0;
				if (++dbi->h_no >= dbi->db->num_hash_tables)
					return 0;
			}
		}
		if (fseeko(dbi->db->f,offset,SEEK_SET))
			return KISSDB_ERROR_IO;
		if (fread(kbuf,dbi->db->key_size,1,dbi->db->f) != 1)
			return KISSDB_ERROR_IO;
		if (fread(vbuf,dbi->db->value_size,1,dbi->db->f) != 1)
			return KISSDB_ERROR_IO;
		if (++dbi->h_idx >= dbi->db->hash_table_size) {
			dbi->h_idx = 0;
			++dbi->h_no;
		}
		return 1;
	}

	return 0;
}








#ifdef KISSDB_TEST

#include <inttypes.h>


/* KISSDB 기능을 테스트하기 위한 주 메인 함수입니다. */
int main(int argc,char **argv)
{
	pthread_t thread1, thread2;
    pthread_create(&thread1, NULL, access_database, NULL);
    pthread_create(&thread2, NULL, access_database, NULL);

	uint64_t i,j;
	uint64_t v[8];
	KISSDB db;
	KISSDB_Iterator dbi;
	char got_all_values[10000];
	int q;

	printf("Opening new empty database test.db...\n");

	if (KISSDB_open(&db,"test.db",KISSDB_OPEN_MODE_RWREPLACE,1024,8,sizeof(v))) {
		printf("KISSDB_open failed\n");
		return 1;
	}

	printf("Adding and then re-getting 10000 64-byte values...\n");

	for(i=0;i<10000;++i) {
		for(j=0;j<8;++j)
			v[j] = i;
		if (KISSDB_put(&db,&i,v)) {
			printf("KISSDB_put failed (%"PRIu64")\n",i);
			return 1;
		}
		memset(v,0,sizeof(v));
		if ((q = KISSDB_get(&db,&i,v))) {
			printf("KISSDB_get (1) failed (%"PRIu64") (%d)\n",i,q);
			return 1;
		}
		for(j=0;j<8;++j) {
			if (v[j] != i) {
				printf("KISSDB_get (1) failed, bad data (%"PRIu64")\n",i);
				return 1;
			}
		}
	}

	printf("Getting 10000 64-byte values...\n");

	for(i=0;i<10000;++i) {
		if ((q = KISSDB_get(&db,&i,v))) {
			printf("KISSDB_get (2) failed (%"PRIu64") (%d)\n",i,q);
			return 1;
		}
		for(j=0;j<8;++j) {
			if (v[j] != i) {
				printf("KISSDB_get (2) failed, bad data (%"PRIu64")\n",i);
				return 1;
			}
		}
	}

	printf("Closing and re-opening database in read-only mode...\n");

	KISSDB_close(&db);

	if (KISSDB_open(&db,"test.db",KISSDB_OPEN_MODE_RDONLY,1024,8,sizeof(v))) {
		printf("KISSDB_open failed\n");
		return 1;
	}

	printf("Getting 10000 64-byte values...\n");

	for(i=0;i<10000;++i) {
		if ((q = KISSDB_get(&db,&i,v))) {
			printf("KISSDB_get (3) failed (%"PRIu64") (%d)\n",i,q);
			return 1;
		}
		for(j=0;j<8;++j) {
			if (v[j] != i) {
				printf("KISSDB_get (3) failed, bad data (%"PRIu64")\n",i);
				return 1;
			}
		}
	}

	printf("Iterator test...\n");

	KISSDB_Iterator_init(&db,&dbi);
	i = 0xdeadbeef;
	memset(got_all_values,0,sizeof(got_all_values));
	while (KISSDB_Iterator_next(&dbi,&i,&v) > 0) {
		if (i < 10000)
			got_all_values[i] = 1;
		else {
			printf("KISSDB_Iterator_next failed, bad data (%"PRIu64")\n",i);
			return 1;
		}
	}
	for(i=0;i<10000;++i) {
		if (!got_all_values[i]) {
			printf("KISSDB_Iterator failed, missing value index %"PRIu64"\n",i);
			return 1;
		}
	}

	KISSDB_close(&db);

	printf("All tests OK!\n");

    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

	return 0;
}

/* 
1. main 함수는 프로그램의 진입점입니다.
2. KISSDB_open 함수를 사용하여 test.db라는 이름의 새로운 데이터베이스를 열고 있습니다. 데이터베이스는 1024개의 해시 테이블 엔트리를 가지고 있고, 각 엔트리는 8개의 64비트 값으로 구성되어 있습니다.
3. for 루프를 통해 0부터 9999까지의 값을 데이터베이스에 추가하고, 다시 해당 값을 검색하여 올바른지 확인합니다.
4. 데이터베이스를 닫고 읽기 전용 모드로 다시 엽니다.(데이터베이스를 수정하지 않고 데이터를 읽기만 하기 위해서 = 무결성 유지)
5. 읽기 전용 모드에서 데이터를 검색하여 이전과 동일한 값인지 확인합니다.
6. 이터레이터를 사용하여 데이터베이스의 모든 값에 반복적으로 접근하고, 모든 값을 가져왔는지 확인합니다.
7. 마지막으로 데이터베이스를 닫고 테스트가 성공적으로 완료되었음을 알립니다.
 */

#endif
